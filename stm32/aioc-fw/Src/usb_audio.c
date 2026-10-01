#include <io.h>
#include "usb_audio.h"
#include "stm32f3xx_hal.h"
#include "aioc.h"
#include "tusb.h"
#include "usb.h"
#include "cos.h"
#include "tx_eq.h"
#include "rx_eq.h"
#include "diag.h"
#include "audio_block.h"
#include <math.h>
#include <string.h>

/* The one and only supported sample rate */
#define DEFAULT_SAMPLE_RATE   	48000
/* This is feedback average responsivity with a denominator of 65536 */
#define SPEAKER_FEEDBACK_AVG    32
/* This is buffer level average responsivity with a denominator of 65536 */
#define SPEAKER_BUFFERLVL_AVG   64
/* This is the amount of buffer level to feedback coupling with a denominator of 65536 to prevent buffer drift */
#define SPEAKER_BUFLVL_FB_COUPLING 1
/* We try to stay on this target with the buffer level: 5 frames, i.e. 5ms at full-speed USB and maximum sample rate,
 * as upstream, plus one block. The level counts what the DMA buffer still holds as well as the USB FIFO, and the
 * DMA takes a block from the FIFO at a time, so the extra block keeps the FIFO as far from running dry as before */
#define SPEAKER_BUFFERLVL_TARGET (5 * CFG_TUD_AUDIO_EP_SZ_OUT)

/* Playback by DMA: TIM6 triggers the DAC, and at every trigger DMA2 channel 3 (DAC1 channel
 * 1's request, not remapped) loads the next sample from dacBuf, circular. dacBuf has two
 * halves of one block (1 ms) each; when the DMA moves on from one half, its interrupt refills
 * that half with the next block from USB */
#define DAC_DMA             DMA2
#define DAC_DMA_CH          DMA2_Channel3
#define DAC_DMA_IRQn        DMA2_Channel3_IRQn
#define DAC_DMA_IRQHandler  DMA2_Channel3_IRQHandler
#define DAC_DMA_IFCR_ALL    DMA_IFCR_CGIF3

/* Recording by DMA: TIM3 triggers the ADC at the recording rate, and the DMA stores every
 * result in adcBuf, circular, two halves of one block (1 ms) each; when the DMA moves on from
 * one half, its interrupt processes that half and hands it to USB. ADC2 (direct input) has
 * its requests on DMA2 channel 1, ADC1 (behind the OPAMP PGA) on DMA1 channel 1; only the one
 * RX_Config picks runs */
#define ADC1_DMA            DMA1
#define ADC1_DMA_CH         DMA1_Channel1
#define ADC1_DMA_IRQn       DMA1_Channel1_IRQn
#define ADC2_DMA            DMA2
#define ADC2_DMA_CH         DMA2_Channel1
#define ADC2_DMA_IRQn       DMA2_Channel1_IRQn
#define ADC_DMA_IFCR_ALL    DMA_IFCR_CGIF1      /* both on channel 1 of their controller */


typedef enum {
    SAMPLERATE_48000, /* The high-quality default */
    SAMPLERATE_32000, /* For completeness sake, support 32 kHz as well */
    SAMPLERATE_24000, /* Just half of 48 kHz */
    SAMPLERATE_22050, /* For APRSdroid support. NOTE: Has approx. 90 ppm of clock frequency error (ca. 22052 Hz) */
    SAMPLERATE_16000, /* On ARM platforms, direwolf will by default, divide configured sample rate by 3, thus support 16 kHz */
    SAMPLERATE_12000, /* Just a quarter of 48 kHz */
    SAMPLERATE_11025, /* NOTE: Has approx. 90 ppm of clock frequency error (ca. 11026 Hz) */
    SAMPLERATE_8000,
    SAMPLERATE_COUNT /* Has to be last element */
} samplerate_t;

typedef enum {
    STATE_OFF,
    STATE_START,
    STATE_RUN
} state_t;

/* Various state variables. N+1 because 0 is always the master channel */
static bool microphoneMute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1];
static bool speakerMute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];
static int16_t microphoneLogVolume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1] = { [0 ... CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX] = 0 }; /* in dB */
static int16_t speakerLogVolume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1] = { [0 ... CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX] = 0 }; /* in dB */
static uint16_t microphoneLinVolume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1] = { [0 ... CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX] = 65535 }; /* 0.16 format */
static uint16_t speakerLinVolume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1] = { [0 ... CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX] = 65535 }; /* 0.16 format */
static uint32_t microphoneSampleFreq = DEFAULT_SAMPLE_RATE; /* Current (requested) sample rate */
static uint32_t speakerSampleFreq = DEFAULT_SAMPLE_RATE; /* Current (requested) sample rate */
static uint64_t speakerFeedbackAvg; /* 32.32 format */
static uint32_t speakerFeedbackMin;
static uint32_t speakerFeedbackMax;
static uint32_t speakerBufferLvlAvg; /* 16.16 format */
static uint16_t speakerBufferLvlMin;
static uint16_t speakerBufferLvlMax;
static volatile uint32_t microphoneSampleFreqCfg; /* Actual configured sample rate in the timer hardware. May be different from requested for odd sample rates */
static volatile uint32_t speakerSampleFreqCfg; /* Actual configured sample rate in the timer hardware. May be different from requested for odd sample rates */
static volatile state_t microphoneState = STATE_OFF;
static volatile state_t speakerState = STATE_OFF;
static txeq_t txEq; /* Playback equaliser, zero-initialised = bypass */
static uint16_t dacBuf[2 * AUDIO_BLOCK_MAX]; /* Playback DMA buffer, two halves of speakerBlock samples */
static uint32_t speakerBlock = AUDIO_BLOCK_MAX; /* Samples per block, set at playback start */
static uint32_t speakerLevelTarget = SPEAKER_BUFFERLVL_TARGET + 2 * AUDIO_BLOCK_MAX; /* Bytes, see SPEAKER_BUFFERLVL_TARGET */
static volatile uint8_t speakerFilled; /* Half of dacBuf filled most recently */
static uint8_t speakerOwnsDac; /* Playback has the DAC (DMA on, trigger TIM6) ... */
static uint32_t speakerOtherTsel; /* ... and this was its trigger before (TIM15 with the fox hunt on) */
static audio_cycles_t speakerCycles; /* Cycles per playback block */
static uint16_t adcBuf[2 * AUDIO_BLOCK_MAX]; /* Recording DMA buffer, two halves of microphoneBlock samples */
static uint32_t microphoneBlock = AUDIO_BLOCK_MAX; /* Samples per block, set at recording start */
static audio_cycles_t microphoneCycles; /* Cycles per recording block */
static rxeq_t rxEq; /* Recording equaliser, zero-initialised = bypass */
static volatile uint8_t cosVirtualState; /* Virtual COS state, as the TIM17 interrupt last set it */
static volatile uint8_t cosVirtualPending; /* ... and not yet shown (USB_AudioTask) */

/* rx_eq.h has no HAL or settings dependencies; its control and status words are the registers' */
_Static_assert(RXEQ_PROFILE_K5 == SETTINGS_REG_RXEQ_CTRL_PROFILE_K5_ENUM, "RX EQ profile numbers differ");
_Static_assert(RXEQ_STATUS_OVERLOAD_MASK == SETTINGS_REG_INFO_RXEQ_OVERLOAD_MASK
               && RXEQ_STATUS_OVERLOADS_OFFS == SETTINGS_REG_INFO_RXEQ_OVERLOADS_OFFS
               && RXEQ_STATUS_OVERLOADS_MASK == SETTINGS_REG_INFO_RXEQ_OVERLOADS_MASK,
               "RX EQ overload fields differ from the registers");
_Static_assert(RXEQ_STATUS_PROFILE_MASK == SETTINGS_REG_INFO_RXEQ_PROFILE_MASK
               && RXEQ_STATUS_ACTIVE_MASK == SETTINGS_REG_INFO_RXEQ_ACTIVE_MASK
               && RXEQ_STATUS_FADE_MASK == SETTINGS_REG_INFO_RXEQ_FADE_MASK
               && RXEQ_STATUS_CLIPS_OFFS == SETTINGS_REG_INFO_RXEQ_CLIPS_OFFS
               && RXEQ_CYCLES_MAX_OFFS == SETTINGS_REG_INFO_RXEQCYC_MAX_OFFS
               && RXEQ_CYCLES_AVG_OFFS == SETTINGS_REG_INFO_RXEQCYC_AVG_OFFS,
               "RX EQ status fields differ from the registers");

static audio_control_range_4_n_t(SAMPLERATE_COUNT) sampleFreqRng = {
    .wNumSubRanges = SAMPLERATE_COUNT,
    .subrange = {
        [SAMPLERATE_48000] = {.bMin = 48000, .bMax = 48000, .bRes = 0},
        [SAMPLERATE_32000] = {.bMin = 32000, .bMax = 32000, .bRes = 0},
        [SAMPLERATE_24000] = {.bMin = 24000, .bMax = 24000, .bRes = 0},
        [SAMPLERATE_22050] = {.bMin = 22050, .bMax = 22050, .bRes = 0},
        [SAMPLERATE_16000] = {.bMin = 16000, .bMax = 16000, .bRes = 0},
        [SAMPLERATE_12000] = {.bMin = 12000, .bMax = 12000, .bRes = 0},
        [SAMPLERATE_11025] = {.bMin = 11025, .bMax = 11025, .bRes = 0},
        [SAMPLERATE_8000]  = {.bMin =  8000, .bMax =  8000, .bRes = 0},
    }
};

/* Prototypes of static functions */
static void Timer_ADC_Init(void);
static void Timer_DAC_Init(void);
static void ADC_Init(void);
static void DAC_Init(void);
static ADC_TypeDef *RX_Config(usb_audio_rxgain_t rxGain);
static void TX_Config(usb_audio_txboost_t txBoost);
static void Timeout_Timers_Init(void);
static void Speaker_Start(void);
static void Speaker_Stop(void);
static uint16_t Speaker_Level(void);
static void Microphone_Start(ADC_TypeDef *adc);
static void Microphone_Stop(void);


//--------------------------------------------------------------------+
// Application Callback API Implementations
//--------------------------------------------------------------------+

// Invoked when audio class specific set request received for an entity
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const * p_request, uint8_t *pBuff)
{
  (void) rhport;

  // Page 91 in UAC2 specification
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t itf = TU_U16_LOW(p_request->wIndex);
  uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

  TU_ASSERT(itf == ITF_NUM_AUDIO_CONTROL);

  // We do not support any set range requests here, only current value requests
  TU_VERIFY(p_request->bRequest == AUDIO_CS_REQ_CUR);

  if ( entityID == AUDIO_CTRL_ID_MIC_FUNIT )
  {
    switch ( ctrlSel )
    {
      case AUDIO_FU_CTRL_MUTE:
        // Request uses format layout 1
        TU_VERIFY(p_request->wLength == sizeof(audio_control_cur_1_t));

        microphoneMute[channelNum] = ((audio_control_cur_1_t*) pBuff)->bCur;

        /* Update debug register */
        settingsRegMap[SETTINGS_REG_INFO_AUDIO0] =  (settingsRegMap[SETTINGS_REG_INFO_AUDIO0] & ~(SETTINGS_REG_INFO_AUDIO0_RECMUTE0_MASK | SETTINGS_REG_INFO_AUDIO0_RECMUTE1_MASK)) \
                                                | (microphoneMute[0] ? SETTINGS_REG_INFO_AUDIO0_RECMUTE0_MASK : 0) \
                                                | (microphoneMute[1] ? SETTINGS_REG_INFO_AUDIO0_RECMUTE1_MASK : 0);

        TU_LOG2("    Set Mute: %d of channel: %u\r\n", microphoneMute[channelNum], channelNum);
      return true;

      case AUDIO_FU_CTRL_VOLUME:
        // Request uses format layout 2
        TU_VERIFY(p_request->wLength == sizeof(audio_control_cur_2_t));

        microphoneLogVolume[channelNum] = ((audio_control_cur_2_t*) pBuff)->bCur;
        double logVolume = microphoneLogVolume[channelNum] / 256; /* format is 7.8 fixed point */
        microphoneLinVolume[channelNum] = (microphoneLogVolume[channelNum] != 0x8000) ?
                (uint16_t) (65535 * pow(10, logVolume/20) + 0.5) : 0; /* log to linear with rounding */

        settingsRegMap[SETTINGS_REG_INFO_AUDIO3] = ((((uint32_t) microphoneLinVolume[0]) << SETTINGS_REG_INFO_AUDIO3_RECVOL0_OFFS) & SETTINGS_REG_INFO_AUDIO3_RECVOL0_MASK) \
                                               | ((((uint32_t) microphoneLinVolume[1]) << SETTINGS_REG_INFO_AUDIO3_RECVOL1_OFFS) & SETTINGS_REG_INFO_AUDIO3_RECVOL1_MASK);

        TU_LOG2("    Set Volume: %u.%u dB of channel: %u\r\n", microphoneLogVolume[channelNum] / 256, microphoneLogVolume[channelNum] % 256, channelNum);
      return true;

        // Unknown/Unsupported control
      default:
        TU_BREAKPOINT();
      return false;
    }
  }

  if ( entityID == AUDIO_CTRL_ID_SPK_FUNIT )
  {
    switch ( ctrlSel )
    {
      case AUDIO_FU_CTRL_MUTE:
        // Request uses format layout 1
        TU_VERIFY(p_request->wLength == sizeof(audio_control_cur_1_t));

        speakerMute[channelNum] = ((audio_control_cur_1_t*) pBuff)->bCur;

        /* Update debug register */
        settingsRegMap[SETTINGS_REG_INFO_AUDIO0] =  (settingsRegMap[SETTINGS_REG_INFO_AUDIO0] & ~(SETTINGS_REG_INFO_AUDIO0_PLAYMUTE0_MASK | SETTINGS_REG_INFO_AUDIO0_PLAYMUTE1_MASK)) \
                                                | (speakerMute[0] ? SETTINGS_REG_INFO_AUDIO0_PLAYMUTE0_MASK : 0) \
                                                | (speakerMute[1] ? SETTINGS_REG_INFO_AUDIO0_PLAYMUTE1_MASK : 0);

        TU_LOG2("    Set Mute: %d of channel: %u\r\n", speakerMute[channelNum], channelNum);

      return true;

      case AUDIO_FU_CTRL_VOLUME:
        // Request uses format layout 2
        TU_VERIFY(p_request->wLength == sizeof(audio_control_cur_2_t));

        speakerLogVolume[channelNum] = ((audio_control_cur_2_t*) pBuff)->bCur;
        double logVolume = (double) speakerLogVolume[channelNum] / 256; /* format is 7.8 fixed point */
        speakerLinVolume[channelNum] = (speakerLogVolume[channelNum] != 0x8000) ?
                (uint16_t) (65535 * pow(10, logVolume/20) + 0.5) : 0; /* log to linear with rounding */

        /* Update debug register */
        settingsRegMap[SETTINGS_REG_INFO_AUDIO9] = ((((uint32_t) speakerLinVolume[0]) << SETTINGS_REG_INFO_AUDIO9_PLAYVOL0_OFFS) & SETTINGS_REG_INFO_AUDIO9_PLAYVOL0_MASK) \
                                               | ((((uint32_t) speakerLinVolume[1]) << SETTINGS_REG_INFO_AUDIO9_PLAYVOL1_OFFS) & SETTINGS_REG_INFO_AUDIO9_PLAYVOL1_MASK);


        TU_LOG2("    Set Volume: %u.%u dB of channel: %u\r\n", microphoneLogVolume[channelNum] / 256, microphoneLogVolume[channelNum] % 256, channelNum);
      return true;

        // Unknown/Unsupported control
      default:
        TU_BREAKPOINT();
      return false;
    }
  }

  if ( entityID == AUDIO_CTRL_ID_MIC_CLOCK )
  {
    switch ( ctrlSel )
    {
      case AUDIO_CS_CTRL_SAM_FREQ:
        // channelNum is always zero in this case
        switch ( p_request->bRequest )
        {
          case AUDIO_CS_REQ_CUR:
            TU_VERIFY(p_request->wLength == sizeof(audio_control_cur_4_t));
            microphoneSampleFreq = ((audio_control_cur_4_t*) pBuff)->bCur;
            TU_LOG2("    Set Mic. Sample Freq: %lu\r\n", microphoneSampleFreq);

            Timer_ADC_Init();

            /* Update debug register */
            settingsRegMap[SETTINGS_REG_INFO_AUDIO2] = (((uint32_t) microphoneSampleFreqCfg) << SETTINGS_REG_INFO_AUDIO2_RECRATE_OFFS) & SETTINGS_REG_INFO_AUDIO2_RECRATE_MASK;

            return true;

          // Unknown/Unsupported control
          default:
            TU_BREAKPOINT();
            return false;
        }
      break;

      // Unknown/Unsupported control
      default:
        TU_BREAKPOINT();
        return false;
    }
  }

  if ( entityID == AUDIO_CTRL_ID_SPK_CLOCK )
  {
    switch ( ctrlSel )
    {
      case AUDIO_CS_CTRL_SAM_FREQ:
        // channelNum is always zero in this case
        switch ( p_request->bRequest )
        {
          case AUDIO_CS_REQ_CUR:
            TU_VERIFY(p_request->wLength == sizeof(audio_control_cur_4_t));
            speakerSampleFreq = ((audio_control_cur_4_t*) pBuff)->bCur;
            TU_LOG2("    Set Spk. Sample Freq: %lu\r\n", speakerSampleFreq);

            Timer_DAC_Init();

            /* Update debug register */
            settingsRegMap[SETTINGS_REG_INFO_AUDIO8] = (((uint32_t) speakerSampleFreqCfg) << SETTINGS_REG_INFO_AUDIO8_PLAYRATE_OFFS) & SETTINGS_REG_INFO_AUDIO8_PLAYRATE_MASK;

            return true;

          // Unknown/Unsupported control
          default:
            TU_BREAKPOINT();
            return false;
        }
      break;

      // Unknown/Unsupported control
      default:
        TU_BREAKPOINT();
        return false;
    }
  }

  return false;    // Yet not implemented
}

// Invoked when audio class specific get request received for an entity
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const * p_request)
{
  (void) rhport;

  // Page 91 in UAC2 specification
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t itf = TU_U16_LOW(p_request->wIndex);
  uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

  TU_ASSERT(itf == ITF_NUM_AUDIO_CONTROL);

  // Input terminal (Microphone input)
  if (entityID == AUDIO_CTRL_ID_MIC_INPUT)
  {
    switch ( ctrlSel )
    {
      case AUDIO_TE_CTRL_CONNECTOR:
      {
        // The terminal connector control only has a get request with only the CUR attribute.
        audio_desc_channel_cluster_t ret;

        // Those are dummy values for now
        ret.bNrChannels = 1;
        ret.bmChannelConfig = 0;
        ret.iChannelNames = 0;

        TU_LOG2("    Get terminal connector\r\n");

        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, (void*) &ret, sizeof(ret));
      }
      break;

        // Unknown/Unsupported control selector
      default:
        TU_BREAKPOINT();
        return false;
    }
  }

  // Output terminal (Speaker output)
  if (entityID == AUDIO_CTRL_ID_SPK_OUTPUT)
  {
    switch ( ctrlSel )
    {
      case AUDIO_TE_CTRL_CONNECTOR:
      {
        // The terminal connector control only has a get request with only the CUR attribute.
        audio_desc_channel_cluster_t ret;

        // Those are dummy values for now
        ret.bNrChannels = 1;
        ret.bmChannelConfig = 0;
        ret.iChannelNames = 0;

        TU_LOG2("    Get terminal connector\r\n");

        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, (void*) &ret, sizeof(ret));
      }
      break;

        // Unknown/Unsupported control selector
      default:
        TU_BREAKPOINT();
        return false;
    }
  }

  if (entityID == AUDIO_CTRL_ID_SPK_FUNIT)
  {
      switch ( ctrlSel )
      {
        case AUDIO_FU_CTRL_MUTE:
          // Audio control mute cur parameter block consists of only one byte - we thus can send it right away
          // There does not exist a range parameter block for microphoneMute
          TU_LOG2("    Get Mute of channel: %u\r\n", channelNum);
          return tud_control_xfer(rhport, p_request, &speakerMute[channelNum], 1);

        case AUDIO_FU_CTRL_VOLUME:
          switch ( p_request->bRequest )
          {
            case AUDIO_CS_REQ_CUR:
              TU_LOG2("    Get Volume of channel: %u\r\n", channelNum);
              return tud_control_xfer(rhport, p_request, &speakerLogVolume[channelNum], sizeof(speakerLogVolume[channelNum]));

            case AUDIO_CS_REQ_RANGE:
              TU_LOG2("    Get Volume range of channel: %u\r\n", channelNum);

              /* The Volume Control is one of the building blocks of a Feature Unit. A Volume Control must support the
                CUR and RANGE(MIN, MAX, RES) attributes. The settings for the CUR, MIN, and MAX attributes can
                range from +127.9961 dB (0x7FFF) down to -127.9961 dB (0x8001) in steps of 1/256 dB or 0.00390625
                dB (0x0001). The settings for the RES attribute can only have positive values and range from 1/256 dB
                (0x0001) to +127.9961 dB (0x7FFF).
                In addition, code 0x8000, representing silence (i.e., -∞ dB), must always be implemented. However, it
                must never be reported as the MIN attribute value. */

              // Copy values - only for testing - better is version below
              audio_control_range_2_n_t(1) ret;

              /* From 1 (0dB) down to 1/65536 (-96dB) */
              ret.wNumSubRanges = 1;
              ret.subrange[0].bMin = -96 * 256;
              ret.subrange[0].bMax = 0;
              ret.subrange[0].bRes = 1;

              return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, (void*) &ret, sizeof(ret));

              // Unknown/Unsupported control
            default:
              TU_BREAKPOINT();
              return false;
          }
        break;

          // Unknown/Unsupported control
        default:
          TU_BREAKPOINT();
          return false;
      }
  }

  // Feature unit
  if (entityID == AUDIO_CTRL_ID_MIC_FUNIT)
  {
    switch ( ctrlSel )
    {
      case AUDIO_FU_CTRL_MUTE:
        // Audio control microphoneMute cur parameter block consists of only one byte - we thus can send it right away
        // There does not exist a range parameter block for microphoneMute
        TU_LOG2("    Get Mute of channel: %u\r\n", channelNum);
        return tud_control_xfer(rhport, p_request, &microphoneMute[channelNum], 1);

      case AUDIO_FU_CTRL_VOLUME:
        switch ( p_request->bRequest )
        {
          case AUDIO_CS_REQ_CUR:
            TU_LOG2("    Get Volume of channel: %u\r\n", channelNum);
            return tud_control_xfer(rhport, p_request, &microphoneLogVolume[channelNum], sizeof(microphoneLogVolume[channelNum]));

          case AUDIO_CS_REQ_RANGE:
            TU_LOG2("    Get Volume range of channel: %u\r\n", channelNum);

            /* The Volume Control is one of the building blocks of a Feature Unit. A Volume Control must support the
              CUR and RANGE(MIN, MAX, RES) attributes. The settings for the CUR, MIN, and MAX attributes can
              range from +127.9961 dB (0x7FFF) down to -127.9961 dB (0x8001) in steps of 1/256 dB or 0.00390625
              dB (0x0001). The settings for the RES attribute can only have positive values and range from 1/256 dB
              (0x0001) to +127.9961 dB (0x7FFF).
              In addition, code 0x8000, representing silence (i.e., -∞ dB), must always be implemented. However, it
              must never be reported as the MIN attribute value. */

            // Copy values - only for testing - better is version below
            audio_control_range_2_n_t(1) ret;

            /* From 1 (0dB) down to 1/65536 (-96dB) */
            ret.wNumSubRanges = 1;
            ret.subrange[0].bMin = -96 * 256;
            ret.subrange[0].bMax = 0;
            ret.subrange[0].bRes = 1;

            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, (void*) &ret, sizeof(ret));

            // Unknown/Unsupported control
          default:
            TU_BREAKPOINT();
            return false;
        }
      break;

        // Unknown/Unsupported control
      default:
        TU_BREAKPOINT();
        return false;
    }
  }

  // Clock Source unit
  if ( entityID == AUDIO_CTRL_ID_MIC_CLOCK )
  {
    switch ( ctrlSel )
    {
      case AUDIO_CS_CTRL_SAM_FREQ:
        // channelNum is always zero in this case
        switch ( p_request->bRequest )
        {
          case AUDIO_CS_REQ_CUR:
            TU_LOG2("    Get Mic. Sample Freq.\r\n");
            return tud_control_xfer(rhport, p_request, &microphoneSampleFreq, sizeof(microphoneSampleFreq));

          case AUDIO_CS_REQ_RANGE:
            TU_LOG2("    Get Mic. Sample Freq. range\r\n");
            return tud_control_xfer(rhport, p_request, &sampleFreqRng, sizeof(sampleFreqRng));

           // Unknown/Unsupported control
          default:
            TU_BREAKPOINT();
            return false;
        }
      break;

      case AUDIO_CS_CTRL_CLK_VALID:
        // Only cur attribute exists for this request
        TU_LOG2("    Get Mic Sample Freq. valid\r\n");

        uint8_t clkValid = 1;
        return tud_control_xfer(rhport, p_request, &clkValid, sizeof(clkValid));

      // Unknown/Unsupported control
      default:
        TU_BREAKPOINT();
        return false;
    }
  }

  // Clock Source unit
  if ( entityID == AUDIO_CTRL_ID_SPK_CLOCK )
  {
    switch ( ctrlSel )
    {
      case AUDIO_CS_CTRL_SAM_FREQ:
        // channelNum is always zero in this case
        switch ( p_request->bRequest )
        {
          case AUDIO_CS_REQ_CUR:
            TU_LOG2("    Get Spk. Sample Freq.\r\n");
            return tud_control_xfer(rhport, p_request, &speakerSampleFreq, sizeof(speakerSampleFreq));

          case AUDIO_CS_REQ_RANGE:
            TU_LOG2("    Get Spk. Sample Freq. range\r\n");
            return tud_control_xfer(rhport, p_request, &sampleFreqRng, sizeof(sampleFreqRng));

           // Unknown/Unsupported control
          default:
            TU_BREAKPOINT();
            return false;
        }
      break;

      case AUDIO_CS_CTRL_CLK_VALID:
        // Only cur attribute exists for this request
        TU_LOG2("    Get Spk. Sample Freq. valid\r\n");

        uint8_t clkValid = 1;
        return tud_control_xfer(rhport, p_request, &clkValid, sizeof(clkValid));

      // Unknown/Unsupported control
      default:
        TU_BREAKPOINT();
        return false;
    }
  }

  TU_LOG2("  Unsupported entity: %d\r\n", entityID);
  return false;     // Yet not implemented
}

bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting) {
    (void) rhport;
    (void) itf;
    (void) ep_in;
    (void) cur_alt_setting;

    if (microphoneState == STATE_START) {
        /* Start ADC sampling as soon as device stacks starts loading data (will be a ZLP for first frame) */
        uint8_t rxGainSetting = (settingsRegMap[SETTINGS_REG_AUDIO_RX] & SETTINGS_REG_AUDIO_RX_RXGAIN_MASK) >> SETTINGS_REG_AUDIO_RX_RXGAIN_OFFS;
        usb_audio_rxgain_t rxGain =
                (rxGainSetting == SETTINGS_REG_AUDIO_RX_RXGAIN_1X_ENUM) ? USB_AUDIO_RXGAIN_1X :
                (rxGainSetting == SETTINGS_REG_AUDIO_RX_RXGAIN_2X_ENUM) ? USB_AUDIO_RXGAIN_2X :
                (rxGainSetting == SETTINGS_REG_AUDIO_RX_RXGAIN_4X_ENUM) ? USB_AUDIO_RXGAIN_4X :
                (rxGainSetting == SETTINGS_REG_AUDIO_RX_RXGAIN_8X_ENUM) ? USB_AUDIO_RXGAIN_8X :
                (rxGainSetting == SETTINGS_REG_AUDIO_RX_RXGAIN_16X_ENUM) ? USB_AUDIO_RXGAIN_16X :
                USB_AUDIO_RXGAIN_1X;

        Microphone_Start(RX_Config(rxGain));
        microphoneState = STATE_RUN;

        /* Update debug register */
        settingsRegMap[SETTINGS_REG_INFO_AUDIO0] = (settingsRegMap[SETTINGS_REG_INFO_AUDIO0] & ~SETTINGS_REG_INFO_AUDIO0_RECSTATE_MASK)
                                               | (((uint32_t) SETTINGS_REG_INFO_AUDIO0_RECSTATE_RUN_ENUM) << SETTINGS_REG_INFO_AUDIO0_RECSTATE_OFFS);
    }

    return true;
}

bool tud_audio_rx_done_post_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
{
    /* Get number of total bytes buffered: in the FIFO, and in the DMA buffer still to be played */
    uint16_t count = Speaker_Level();

    /* Calculate min/max/average statistics of buffer fill level */
    if ( (count - n_bytes_received) < speakerBufferLvlMin) speakerBufferLvlMin = count - n_bytes_received;
    if ( count > speakerBufferLvlMax) speakerBufferLvlMax = count;
    speakerBufferLvlAvg = ((uint64_t) speakerBufferLvlAvg * (65536 - SPEAKER_BUFFERLVL_AVG) + ((uint64_t) count << 16) * SPEAKER_BUFFERLVL_AVG) / 65536.0;

    if (speakerState == STATE_START) {
        /* The block length follows the playback rate */
        speakerBlock = AudioBlock_Len(speakerSampleFreq);
        speakerLevelTarget = SPEAKER_BUFFERLVL_TARGET + speakerBlock * CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE;

        if (count >= speakerLevelTarget) {
            /* Wait until we are at buffer target fill level, then start DAC output */
            /* Start the TX equaliser afresh with the set in the registers now. The DMA interrupt
             * is still disabled here, so this cannot race with it */
            TxEq_Reset(&txEq, settingsRegMap[SETTINGS_REG_TXEQ_CTRL], &settingsRegMap[SETTINGS_REG_TXEQ_COEF0], speakerSampleFreq);
            TX_Config((settingsRegMap[SETTINGS_REG_AUDIO_TX] & SETTINGS_REG_AUDIO_TX_TXBOOST_MASK) ? USB_AUDIO_TXBOOST_ON : USB_AUDIO_TXBOOST_OFF);
            Speaker_Start();
            speakerState = STATE_RUN;

            /* Update debug register */
            settingsRegMap[SETTINGS_REG_INFO_AUDIO0] = (settingsRegMap[SETTINGS_REG_INFO_AUDIO0] & ~SETTINGS_REG_INFO_AUDIO0_PLAYSTATE_MASK)
                                                   | (((uint32_t) SETTINGS_REG_INFO_AUDIO0_PLAYSTATE_RUN_ENUM) << SETTINGS_REG_INFO_AUDIO0_PLAYSTATE_OFFS);
        }

        /* Initialize/override min/max/avg during startup buffering (the average is 16.16) */
        speakerBufferLvlAvg = (uint32_t) count << 16;
        speakerBufferLvlMin = count;
        speakerBufferLvlMax = count;
    }

    /* Write to debug registers */
    settingsRegMap[SETTINGS_REG_INFO_AUDIO10] = ((uint32_t) (speakerBufferLvlAvg >> 16) << SETTINGS_REG_INFO_AUDIO10_PLAYBUFAVG_OFFS) & SETTINGS_REG_INFO_AUDIO10_PLAYBUFAVG_MASK;
    settingsRegMap[SETTINGS_REG_INFO_AUDIO11] = ((uint32_t) speakerBufferLvlMin         << SETTINGS_REG_INFO_AUDIO11_PLAYBUFMIN_OFFS) & SETTINGS_REG_INFO_AUDIO11_PLAYBUFMIN_MASK;
    settingsRegMap[SETTINGS_REG_INFO_AUDIO12] = ((uint32_t) speakerBufferLvlMax         << SETTINGS_REG_INFO_AUDIO12_PLAYBUFMAX_OFFS) & SETTINGS_REG_INFO_AUDIO12_PLAYBUFMAX_MASK;

    return true;
}


bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const * p_request)
{
    (void) rhport;
    (void) p_request;

    uint16_t itf = p_request->wIndex;
    uint16_t alt = p_request->wValue;

    switch(itf) {
    case ITF_NUM_AUDIO_STREAMING_IN:
        if (alt == 1) {
            /* Microphone channel has been activated. Stop whatever runs first: after a USB
             * bus reset the old stream was never closed */
            Microphone_Stop();
            microphoneState = STATE_START;

            /* Update VCOS/VPTT timeouts */
            Timeout_Timers_Init();

            /* Update debug register */
            settingsRegMap[SETTINGS_REG_INFO_AUDIO0] = (settingsRegMap[SETTINGS_REG_INFO_AUDIO0] & ~SETTINGS_REG_INFO_AUDIO0_RECSTATE_MASK)
                                                   | (((uint32_t) SETTINGS_REG_INFO_AUDIO0_RECSTATE_START_ENUM) << SETTINGS_REG_INFO_AUDIO0_RECSTATE_OFFS);
        }
        break;

    case ITF_NUM_AUDIO_STREAMING_OUT:
        if (alt == 1) {
            /* Speaker channel has been activated. Stop whatever runs first, as above: a
             * playback DMA left running would drain the FIFO, so the new stream would never
             * reach its start level */
            Speaker_Stop();
            speakerState = STATE_START;

            /* Update VCOS/VPTT timeouts */
            Timeout_Timers_Init();

            /* Update debug register */
            settingsRegMap[SETTINGS_REG_INFO_AUDIO0] = (settingsRegMap[SETTINGS_REG_INFO_AUDIO0] & ~SETTINGS_REG_INFO_AUDIO0_PLAYSTATE_MASK)
                                                   | (((uint32_t) SETTINGS_REG_INFO_AUDIO0_PLAYSTATE_START_ENUM) << SETTINGS_REG_INFO_AUDIO0_PLAYSTATE_OFFS);
        }
        break;

    default:
        TU_ASSERT(0, false);
        break;
    }

    return true;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void) rhport;
    (void) p_request;

    uint16_t itf = p_request->wIndex;

    switch (itf) {
    case ITF_NUM_AUDIO_STREAMING_IN:
        /* Microphone channel has been stopped */
        Microphone_Stop();
        microphoneState = STATE_OFF;

        /* Update debug register */
        settingsRegMap[SETTINGS_REG_INFO_AUDIO0] = (settingsRegMap[SETTINGS_REG_INFO_AUDIO0] & ~SETTINGS_REG_INFO_AUDIO0_RECSTATE_MASK)
                                               | (((uint32_t) SETTINGS_REG_INFO_AUDIO0_RECSTATE_OFF_ENUM) << SETTINGS_REG_INFO_AUDIO0_RECSTATE_OFFS);
        break;

    case ITF_NUM_AUDIO_STREAMING_OUT:
        /* Speaker channel has been stopped */
        Speaker_Stop();
        speakerState = STATE_OFF;

        /* Update debug register */
        settingsRegMap[SETTINGS_REG_INFO_AUDIO0] = (settingsRegMap[SETTINGS_REG_INFO_AUDIO0] & ~SETTINGS_REG_INFO_AUDIO0_PLAYSTATE_MASK)
                                               | (((uint32_t) SETTINGS_REG_INFO_AUDIO0_PLAYSTATE_OFF_ENUM) << SETTINGS_REG_INFO_AUDIO0_PLAYSTATE_OFFS);
        break;

    default:
        TU_ASSERT(0, false);
        break;
    }

    return true;
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t* feedback_param)
{
    /* Configure parameters for feedback endpoint */
    feedback_param->frequency.mclk_freq = USB_SOF_TIMER_HZ;
    feedback_param->sample_freq = speakerSampleFreqCfg;
    feedback_param->method = AUDIO_FEEDBACK_METHOD_FREQUENCY_FIXED;
}

bool tud_audio_feedback_format_correction_cb(uint8_t func_id)
{
    /* Use the quirk detection to detect whether we need format correction (10.14) according to the USB specification (MacOS)
     * or whether we use no correction (16.16) as a quirk (Windows). Linux works either way. */
    if (tud_speed_get() == TUSB_SPEED_FULL) {
        return USB_DescUAC2Quirk() ? false : true;
    } else {
        return false;
    }
}

TU_ATTR_FAST_FUNC void tud_audio_feedback_interval_isr(uint8_t func_id, uint32_t frame_number, uint8_t interval_shift)
{
    static uint32_t prev_cycles = 0;
    uint32_t this_cycles = USB_SOF_TIMER_CNT;
    uint32_t feedback;

    /* Calculate number of master clock cycles between now and last call */
    uint32_t cycles = (uint32_t) (((uint64_t) this_cycles - prev_cycles) & 0xFFFFFFFFUL);
    TU_ASSERT(cycles != 0, /**/);
    /* Prepare for next time */
    prev_cycles = this_cycles;

    /* Calculate the feedback value, taken from tinyusb stack */
    uint64_t fb64 = (((uint64_t) cycles) * speakerSampleFreqCfg) << 16;
    feedback = (uint32_t) (fb64 / USB_SOF_TIMER_HZ);

    /* Couple the buffer level bias to the feedback value to avoid buffer drift */
    if (speakerState == STATE_RUN) {
        int32_t bias = (int32_t) speakerBufferLvlAvg - ((int32_t) speakerLevelTarget << 16); /* 16.16 format same as feedback */
        feedback -= ((int64_t) bias * SPEAKER_BUFLVL_FB_COUPLING) / 65536;
    }

    /* The size of isochronous packets created by the device must be within the limits specified in FMT-2.0 section 2.3.1.1.
     * This means that the deviation of actual packet size from nominal size must not exceed +/- one audio slot
     * (audio slot = channel count samples). */
    uint32_t sampleFreq = speakerSampleFreq;
    uint32_t min_value = (sampleFreq/1000 - 1) << 16; /* 1000 for full-speed USB */
    uint32_t max_value = (sampleFreq/1000 + 1) << 16;

    /* Limit */
    if ( feedback > max_value ) feedback = max_value;
    if ( feedback < min_value ) feedback = min_value;

    /* Send to host */
    tud_audio_n_fb_set(func_id, feedback);

    /* Handle min/max/avg statistics */
    if (feedback < speakerFeedbackMin) speakerFeedbackMin = feedback;
    if (feedback > speakerFeedbackMax) speakerFeedbackMax = feedback;
    speakerFeedbackAvg = (speakerFeedbackAvg * (65536 - SPEAKER_FEEDBACK_AVG) + ((uint64_t) feedback << 16) * SPEAKER_FEEDBACK_AVG) / 65536.0;

    if (speakerState == STATE_START) {
        /* Initialize/overwrite min/max/avg during start */
        speakerFeedbackAvg = (uint64_t) feedback << 16;
        speakerFeedbackMin = feedback;
        speakerFeedbackMax = feedback;
    }

    /* Write to debug registers */
    settingsRegMap[SETTINGS_REG_INFO_AUDIO13] = ((uint32_t) (speakerFeedbackAvg >> 16) << SETTINGS_REG_INFO_AUDIO13_PLAYFBAVG_OFFS) & SETTINGS_REG_INFO_AUDIO13_PLAYFBAVG_MASK;
    settingsRegMap[SETTINGS_REG_INFO_AUDIO14] = ((uint32_t) speakerFeedbackMin         << SETTINGS_REG_INFO_AUDIO14_PLAYFBMIN_OFFS) & SETTINGS_REG_INFO_AUDIO14_PLAYFBMIN_MASK;
    settingsRegMap[SETTINGS_REG_INFO_AUDIO15] = ((uint32_t) speakerFeedbackMax         << SETTINGS_REG_INFO_AUDIO15_PLAYFBMAX_OFFS) & SETTINGS_REG_INFO_AUDIO15_PLAYFBMAX_MASK;
}

/* One recording block, from the half of adcBuf the DMA is not in: COS check, RX equaliser and
 * volume, then into the USB FIFO */
static void Microphone_Block(uint32_t half)
{
    uint32_t n = microphoneBlock;
    int16_t block[AUDIO_BLOCK_MAX];

    AudioBlock_FromAdc(block, &adcBuf[half * n], n);

    /* Automatic COS */
    uint16_t cosThreshold = (settingsRegMap[SETTINGS_REG_VCOS_LVLCTRL] & SETTINGS_REG_VCOS_LVLCTRL_THRSHLD_MASK) >> SETTINGS_REG_VCOS_LVLCTRL_THRSHLD_OFFS;

    if (!microphoneMute[1] && AudioBlock_Loud(block, n, cosThreshold)) {
        /* Reset timeout and make sure timer is enabled */
        TIM17->EGR = TIM_EGR_UG; /* Generate an update event in the timer */
    }

    /* RX equaliser, on the raw samples (COS above has seen them unfiltered) and before the
     * volume. Switches over with a crossfade when the control register changed, passes the
     * samples through untouched when bypassed. Its cost is measured on the cycle counter */
    uint32_t eqStart = DWT->CYCCNT;
    RxEq_Poll(&rxEq, settingsRegMap[SETTINGS_REG_RXEQ_CTRL], microphoneSampleFreq);
    RxEq_ProcessBlock(&rxEq, block, n);

    /* Cycle statistics, and the overload guard: if the equaliser ever leaves the main loop
     * (which refreshes the watchdog) no time, or runs far over its budget, it switches
     * itself off. The status registers are written by the main loop (USB_AudioTask) */
    RxEq_AccountBlock(&rxEq, DWT->CYCCNT - eqStart, n, mainLoopPasses);

    /* Scale with 16-bit unsigned volume and round */
    AudioBlock_Volume(block, n, !microphoneMute[1] ? microphoneLinVolume[1] : 0);

    /* Store in FIFO */
    tud_audio_write(block, n * sizeof(int16_t));
}

static void Microphone_Dma(DMA_TypeDef *dma, DMA_Channel_TypeDef *ch)
{
    uint32_t start = DWT->CYCCNT;
    DIAG_CRUMB(adcTick);

    /* Half transfer or transfer complete: the DMA has moved on to the other half. Take the
     * one it is not in (from its position, so a late interrupt cannot pick the wrong one) */
    dma->IFCR = ADC_DMA_IFCR_ALL;
    Microphone_Block(AudioBlock_FreeHalf(ch->CNDTR, microphoneBlock));

    AudioCycles_Count(&microphoneCycles, DWT->CYCCNT - start, microphoneBlock);
    settingsRegMap[SETTINGS_REG_INFO_RXCYC] = AudioCycles_Word(&microphoneCycles);
}

/* An ADC overrun (a result not taken before the next) blocks the ADC's DMA requests until it is
 * cleared, which would stop recording for good. The DMA takes every result long before the next
 * conversion, so this should never run; if it does, recording carries on one sample short */
void ADC1_2_IRQHandler(void)
{
    ADC1->ISR = ADC_ISR_OVR;
    ADC2->ISR = ADC_ISR_OVR;
}

void DMA1_Channel1_IRQHandler(void)
{
    Microphone_Dma(ADC1_DMA, ADC1_DMA_CH);
}

void DMA2_Channel1_IRQHandler(void)
{
    Microphone_Dma(ADC2_DMA, ADC2_DMA_CH);
}

/* Stop an ADC's conversions, if it is converting, and wait until it has */
static void ADC_Halt(ADC_TypeDef *adc)
{
    if (adc->CR & ADC_CR_ADSTART) {
        adc->CR |= ADC_CR_ADSTP;
        for (uint32_t i = 0; (i < 100000) && (adc->CR & ADC_CR_ADSTP); i++)
            ;
    }
}

static void Microphone_Start(ADC_TypeDef *adc)
{
    DMA_TypeDef *dma = (adc == ADC1) ? ADC1_DMA : ADC2_DMA;
    DMA_Channel_TypeDef *ch = (adc == ADC1) ? ADC1_DMA_CH : ADC2_DMA_CH;
    IRQn_Type irq = (adc == ADC1) ? ADC1_DMA_IRQn : ADC2_DMA_IRQn;

    /* Both ADCs and DMAs stopped first, so the DMA starts at the beginning of adcBuf with the
     * ADC's first conversion, and no interrupt runs while the equaliser is reset */
    Microphone_Stop();

    /* The block length follows the recording rate */
    microphoneBlock = AudioBlock_Len(microphoneSampleFreq);
    AudioCycles_Reset(&microphoneCycles);

    /* Start the RX equaliser afresh with the profile in the register now */
    RxEq_Reset(&rxEq, settingsRegMap[SETTINGS_REG_RXEQ_CTRL], microphoneSampleFreq);

    /* Peripheral to memory, 16 bit, high priority */
    ch->CPAR = (uint32_t) &adc->DR;
    ch->CMAR = (uint32_t) adcBuf;
    ch->CNDTR = 2 * microphoneBlock;
    ch->CCR = DMA_CCR_PL_1 | DMA_CCR_MSIZE_0 | DMA_CCR_PSIZE_0 | DMA_CCR_MINC | DMA_CCR_CIRC
            | DMA_CCR_HTIE | DMA_CCR_TCIE;
    ch->CCR |= DMA_CCR_EN;
    dma->IFCR = ADC_DMA_IFCR_ALL;

    NVIC_ClearPendingIRQ(irq);
    NVIC_EnableIRQ(irq);

    /* Clear the ADC's flags (an overrun from before would hold off its DMA requests), then
     * start it: it converts at every TIM3 trigger from now on */
    adc->ISR = ADC_ISR_OVR | ADC_ISR_EOS | ADC_ISR_EOC | ADC_ISR_EOSMP;
    NVIC_ClearPendingIRQ(ADC1_2_IRQn);
    NVIC_EnableIRQ(ADC1_2_IRQn);
    adc->CR |= ADC_CR_ADSTART;
}

static void Microphone_Stop(void)
{
    NVIC_DisableIRQ(ADC1_DMA_IRQn);
    NVIC_DisableIRQ(ADC2_DMA_IRQn);
    NVIC_DisableIRQ(ADC1_2_IRQn);

    ADC_Halt(ADC1);
    ADC_Halt(ADC2);

    ADC1_DMA_CH->CCR &= ~DMA_CCR_EN;
    ADC2_DMA_CH->CCR &= ~DMA_CCR_EN;
    ADC1_DMA->IFCR = ADC_DMA_IFCR_ALL;
    ADC2_DMA->IFCR = ADC_DMA_IFCR_ALL;
    NVIC_ClearPendingIRQ(ADC1_DMA_IRQn);
    NVIC_ClearPendingIRQ(ADC2_DMA_IRQn);
}

/* One playback block: read it from USB, VPTT check, volume and TX equaliser, then into the
 * half of dacBuf the DMA is not in */
static void Speaker_Fill(uint32_t half)
{
    uint32_t n = speakerBlock;
    int16_t block[AUDIO_BLOCK_MAX];

    /* Read from FIFO, leave the rest at 0 if the FIFO runs empty */
    uint32_t got = tud_audio_read(block, n * sizeof(int16_t));
    memset((uint8_t *) block + got, 0, n * sizeof(int16_t) - got);

    /* Automatic PTT */
    uint16_t pttThreshold = (settingsRegMap[SETTINGS_REG_VPTT_LVLCTRL] & SETTINGS_REG_VPTT_LVLCTRL_THRSHLD_MASK) >> SETTINGS_REG_VPTT_LVLCTRL_THRSHLD_OFFS;

    if (!speakerMute[1] && AudioBlock_Loud(block, n, pttThreshold)) {
        /* Reset timeout and make sure timer is enabled */
        TIM16->EGR = TIM_EGR_UG; /* Generate an update event in the timer */
    }

    /* Scale with 16-bit unsigned volume and round */
    AudioBlock_Volume(block, n, !speakerMute[1] ? speakerLinVolume[1] : 0);

    /* TX equaliser. Latches new coefficients when the control register changed, passes the
     * samples through untouched when bypassed */
    TxEq_Poll(&txEq, settingsRegMap[SETTINGS_REG_TXEQ_CTRL], &settingsRegMap[SETTINGS_REG_TXEQ_COEF0], speakerSampleFreq);
    TxEq_ProcessBlock(&txEq, block, n);
    settingsRegMap[SETTINGS_REG_INFO_TXEQ] = TxEq_Status(&txEq);

    /* Into the DMA buffer */
    AudioBlock_ToDac(&dacBuf[half * n], block, n);
    speakerFilled = half;
}

void DAC_DMA_IRQHandler(void)
{
    uint32_t start = DWT->CYCCNT;
    DIAG_CRUMB(dacTick);

    /* Half transfer or transfer complete: the DMA has moved on to the other half. Fill the
     * one it is not in (from its position, so a late interrupt cannot pick the wrong one) */
    DAC_DMA->IFCR = DAC_DMA_IFCR_ALL;
    Speaker_Fill(AudioBlock_FreeHalf(DAC_DMA_CH->CNDTR, speakerBlock));

    AudioCycles_Count(&speakerCycles, DWT->CYCCNT - start, speakerBlock);
    settingsRegMap[SETTINGS_REG_INFO_TXCYC] = AudioCycles_Word(&speakerCycles);
}

/* A DAC DMA underrun (a trigger before the DMA served the last one) stops the DAC's DMA
 * requests for good, and playback would hold one value. The DMA serves every request long
 * before the next trigger, so this should never run; if it does, playback restarts from two
 * fresh blocks (the TX equaliser carries on). TIM6's own update interrupt is off */
void TIM6_DAC_IRQHandler(void)
{
    if (DAC->SR & DAC_SR_DMAUDR1) {
        DAC->SR = DAC_SR_DMAUDR1;
        if (speakerState == STATE_RUN) {
            Speaker_Start();
        } else {
            Speaker_Stop();
        }
    }
}

static void Speaker_Start(void)
{
    /* Stopped first, in case a stream was never closed (a USB reset), so nothing else runs
     * Speaker_Fill. Both halves next, so the DMA starts on audio, then the DMA, then the
     * DAC's requests */
    Speaker_Stop();
    AudioCycles_Reset(&speakerCycles);
    Speaker_Fill(0);
    Speaker_Fill(1);

    DAC_DMA_CH->CCR = 0;
    DAC_DMA_CH->CPAR = (uint32_t) &DAC1->DHR12L1;
    DAC_DMA_CH->CMAR = (uint32_t) dacBuf;
    DAC_DMA_CH->CNDTR = 2 * speakerBlock;
    /* Memory to peripheral, 16 bit samples into the 32 bit register (zero extended), high priority */
    DAC_DMA_CH->CCR = DMA_CCR_PL_1 | DMA_CCR_MSIZE_0 | DMA_CCR_PSIZE_1 | DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_DIR
                    | DMA_CCR_HTIE | DMA_CCR_TCIE;
    DAC_DMA_CH->CCR |= DMA_CCR_EN;

    NVIC_ClearPendingIRQ(DAC_DMA_IRQn);
    NVIC_EnableIRQ(DAC_DMA_IRQn);

    /* The DAC: triggered by TIM6, at the playback rate (the fox hunt sets TIM15, which
     * Speaker_Stop gives back), a stale underrun cleared, then its DMA requests, and an
     * interrupt if one is ever missed (TIM6_DAC_IRQHandler) */
    speakerOtherTsel = DAC->CR & DAC_CR_TSEL1;
    speakerOwnsDac = 1;
    DAC->SR = DAC_SR_DMAUDR1;
    DAC->CR = (DAC->CR & ~DAC_CR_TSEL1) | DAC_CR_DMAEN1 | DAC_CR_DMAUDRIE1;
}

static void Speaker_Stop(void)
{
    NVIC_DisableIRQ(DAC_DMA_IRQn);
    DAC->CR &= ~(DAC_CR_DMAEN1 | DAC_CR_DMAUDRIE1);
    if (speakerOwnsDac) {
        /* Give the DAC its trigger back */
        DAC->CR = (DAC->CR & ~DAC_CR_TSEL1) | speakerOtherTsel;
        speakerOwnsDac = 0;
    }
    DAC_DMA_CH->CCR &= ~DMA_CCR_EN;
    DAC_DMA->IFCR = DAC_DMA_IFCR_ALL;
    NVIC_ClearPendingIRQ(DAC_DMA_IRQn);

    /* Output VDD/2 */
    DAC1->DHR12L1 = 32768;
}

/* Playback buffered, in bytes: the USB FIFO and what the DMA has still to play. Read together
 * with interrupts held off, so a block moving from one to the other cannot be missed or
 * counted twice */
static uint16_t Speaker_Level(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    uint32_t level = tud_audio_available();
    if (speakerState == STATE_RUN) {
        level += AudioBlock_PlayPending(DAC_DMA_CH->CNDTR, speakerBlock, speakerFilled) * CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE;
    }

    __set_PRIMASK(primask);
    return (uint16_t) level;
}

void TIM16_IRQHandler(void)
{
    /* This is a timeout counter for the automatic PTT function */
    uint32_t flags = TIM16->SR;

    if (flags & TIM_SR_UIF) {
        /* Timer was reset (via the EGR register). */
        uint32_t cr = TIM16->CR1;
        if (!(cr & TIM_CR1_CEN)) {
            /* If timer was not enabled previously, enable timer and assert PTT */
            TIM16->CR1 = cr | TIM_CR1_CEN;

            /* Update debug register */
            settingsRegMap[SETTINGS_REG_INFO_AIOC0] |= SETTINGS_REG_INFO_AIOC0_VPTTSTATE_MASK;

            /* Assert enabled PTTs */
            uint8_t pttMask = IO_PTT_MASK_NONE;
            pttMask |= settingsRegMap[SETTINGS_REG_AIOC_IOMUX0] & SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_VPTT_MASK ? IO_PTT_MASK_PTT1 : 0;
            pttMask |= settingsRegMap[SETTINGS_REG_AIOC_IOMUX1] & SETTINGS_REG_AIOC_IOMUX1_OUT2SRC_VPTT_MASK ? IO_PTT_MASK_PTT2 : 0;

            IO_PTTAssert(pttMask);
        }
    } else if (flags & TIM_SR_CC1IF) {
        /* The idle timeout (without any action on the DAC) was reached. Disable timer and deassert PTT */
        TIM16->CR1 &= ~TIM_CR1_CEN;

        /* Update debug register */
        settingsRegMap[SETTINGS_REG_INFO_AIOC0] &= ~SETTINGS_REG_INFO_AIOC0_VPTTSTATE_MASK;

        /* Deassert enabled PTTs */
        uint8_t pttMask = IO_PTT_MASK_NONE;
        pttMask |= settingsRegMap[SETTINGS_REG_AIOC_IOMUX0] & SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_VPTT_MASK ? IO_PTT_MASK_PTT1 : 0;
        pttMask |= settingsRegMap[SETTINGS_REG_AIOC_IOMUX1] & SETTINGS_REG_AIOC_IOMUX1_OUT2SRC_VPTT_MASK ? IO_PTT_MASK_PTT2 : 0;

        IO_PTTDeassert(pttMask);
    }

    TIM16->SR = ~flags;
}

void TIM17_IRQHandler(void)
{
    /* This is a timeout counter for the automatic COS function */
    uint32_t flags = TIM17->SR;

    if (flags & TIM_SR_UIF) {
        /* Timer was reset (via the EGR register). */
        uint32_t cr = TIM17->CR1;
        if (!(cr & TIM_CR1_CEN)) {
            /* If timer was not enabled previously, enable timer and notify host of COS */
            TIM17->CR1 = cr | TIM_CR1_CEN;

            /* Update debug register */
            settingsRegMap[SETTINGS_REG_INFO_AIOC0] |= SETTINGS_REG_INFO_AIOC0_VCOSSTATE_MASK;

            /* Set COS state, shown from the main loop (USB_AudioTask): the HID report must not be
             * sent from an interrupt, which could cut into tinyusb anywhere */
            cosVirtualState = 0x01;
            cosVirtualPending = 1;
        }
    } else if (flags & TIM_SR_CC1IF) {
        /* The idle timeout (without any action on the ADC) was reached. Disable timer and notify host */
        TIM17->CR1 &= ~TIM_CR1_CEN;

        /* Update debug register */
        settingsRegMap[SETTINGS_REG_INFO_AIOC0] &= ~SETTINGS_REG_INFO_AIOC0_VCOSSTATE_MASK;

        /* Set COS state, shown from the main loop as above */
        cosVirtualState = 0x00;
        cosVirtualPending = 1;
    }

    TIM17->SR = ~flags;
}

static void GPIO_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitTypeDef ADCInGpio;
    ADCInGpio.Pin = GPIO_PIN_2;
    ADCInGpio.Mode = GPIO_MODE_ANALOG;
    ADCInGpio.Pull = GPIO_NOPULL;
    ADCInGpio.Speed = GPIO_SPEED_FREQ_LOW;
    ADCInGpio.Alternate = 0;
    HAL_GPIO_Init(GPIOB, &ADCInGpio);

    GPIO_InitTypeDef OPAMP1InGpio;
    OPAMP1InGpio.Pin = GPIO_PIN_5;
    OPAMP1InGpio.Mode = GPIO_MODE_ANALOG;
    OPAMP1InGpio.Pull = GPIO_NOPULL;
    OPAMP1InGpio.Speed = GPIO_SPEED_FREQ_LOW;
    OPAMP1InGpio.Alternate = 0;
    HAL_GPIO_Init(GPIOA, &OPAMP1InGpio);

    GPIO_InitTypeDef OPAMP2OutGpio;
    OPAMP2OutGpio.Pin = GPIO_PIN_6;
    OPAMP2OutGpio.Mode = GPIO_MODE_ANALOG;
    OPAMP2OutGpio.Pull = GPIO_NOPULL;
    OPAMP2OutGpio.Speed = GPIO_SPEED_FREQ_LOW;
    OPAMP2OutGpio.Alternate = 0;
    HAL_GPIO_Init(GPIOA, &OPAMP2OutGpio);

    GPIO_InitTypeDef SamplerateGpio;
    SamplerateGpio.Pin = GPIO_PIN_0;
    SamplerateGpio.Mode = GPIO_MODE_AF_PP;
    SamplerateGpio.Pull = GPIO_NOPULL;
    SamplerateGpio.Speed = GPIO_SPEED_FREQ_HIGH;
    SamplerateGpio.Alternate = GPIO_AF2_TIM3;
    HAL_GPIO_Init(GPIOB, &SamplerateGpio);

    GPIO_InitTypeDef DACOutGpio;
    DACOutGpio.Pin = GPIO_PIN_4;
    DACOutGpio.Mode = GPIO_MODE_ANALOG;
    DACOutGpio.Pull = GPIO_NOPULL;
    DACOutGpio.Speed = GPIO_SPEED_FREQ_LOW;
    DACOutGpio.Alternate = 0;
    HAL_GPIO_Init(GPIOA, &DACOutGpio);

    GPIO_InitTypeDef DACAttenGpio;
    DACAttenGpio.Pin = GPIO_PIN_3;
    DACAttenGpio.Mode = GPIO_MODE_OUTPUT_OD;
    DACAttenGpio.Pull = GPIO_NOPULL;
    DACAttenGpio.Speed = GPIO_SPEED_FREQ_LOW;
    DACAttenGpio.Alternate = 0;
    HAL_GPIO_Init(GPIOA, &DACAttenGpio);
}

static void Timer_ADC_Init(void)
{
	/* Calculate clock rate divider for requested sample rate with rounding */
	uint32_t timerFreq = (HAL_RCC_GetHCLKFreq() == HAL_RCC_GetPCLK1Freq()) ? HAL_RCC_GetPCLK1Freq() : 2 * HAL_RCC_GetPCLK1Freq();
	uint32_t rateDivider = (timerFreq + microphoneSampleFreq / 2) / microphoneSampleFreq;

	/* Store actually realized samplerate */
	microphoneSampleFreqCfg = timerFreq / rateDivider;

	/* Enable clock and (re-) initialize timer */
    __HAL_RCC_TIM3_CLK_ENABLE();

    /* TIM3_TRGO triggers ADC2 */
    TIM3->CR1 &= ~TIM_CR1_CEN;
    TIM3->CR1 = TIM_CLOCKDIVISION_DIV1 | TIM_COUNTERMODE_UP | TIM_AUTORELOAD_PRELOAD_ENABLE;
    TIM3->CR2 = TIM_TRGO_UPDATE;
    TIM3->PSC = 0;
    TIM3->ARR = rateDivider - 1;
    TIM3->EGR = TIM_EGR_UG;
#if 1 /* Output sample rate on compare channel 3 */
    TIM3->CCMR2 =  TIM_OCMODE_PWM1 | TIM_CCMR2_OC3PE;
    TIM3->CCER = (0 << TIM_CCER_CC3P_Pos) | TIM_CCER_CC3E;
    TIM3->CCR3 = rateDivider/2 - 1;
#endif
    TIM3->CR1 |= TIM_CR1_CEN;
}

static void Timer_DAC_Init(void)
{
    /* Calculate clock rate divider for requested sample rate with rounding */
    uint32_t timerFreq = (HAL_RCC_GetHCLKFreq() == HAL_RCC_GetPCLK1Freq()) ? HAL_RCC_GetPCLK1Freq() : 2 * HAL_RCC_GetPCLK1Freq();
    uint32_t rateDivider = (timerFreq + speakerSampleFreq / 2) / speakerSampleFreq;

    /* Store actually realized samplerate for feedback algorithm to use */
    speakerSampleFreqCfg = timerFreq / rateDivider;

    /* Enable clock and (re-) initialize timer */
    __HAL_RCC_TIM6_CLK_ENABLE();

    /* TIM6_TRGO triggers DAC */
    TIM6->CR1 &= ~TIM_CR1_CEN;
    TIM6->CR1 = TIM_CLOCKDIVISION_DIV1 | TIM_COUNTERMODE_UP | TIM_AUTORELOAD_PRELOAD_ENABLE;
    TIM6->CR2 = TIM_TRGO_UPDATE;
    TIM6->PSC = 0;
    TIM6->ARR = rateDivider - 1;
    TIM6->EGR = TIM_EGR_UG;

    TIM6->DIER = 0; /* Only its trigger: the DMA feeds the DAC */
    TIM6->CR1 |= TIM_CR1_CEN;
}

static void ADC_Init(void)
{
    /* We use two ADCs. ADC1 is used, when OPAMP(PGA) is used. Otherwise ADC2 with direct hardware connection is used */
    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_ADC2_CLK_ENABLE();

    ADC1->CR = 0x00 << ADC_CR_ADVREGEN_Pos;
    ADC2->CR = 0x00 << ADC_CR_ADVREGEN_Pos;
    ADC1->CR = 0x01 << ADC_CR_ADVREGEN_Pos;
    ADC2->CR = 0x01 << ADC_CR_ADVREGEN_Pos;

    for (uint32_t i=0; i<200; i++) {
        asm volatile ("nop");
    }

    /* Select AHB clock */
    ADC12_COMMON->CCR = (0x1 << ADC12_CCR_CKMODE_Pos) | (0x00 << ADC12_CCR_MULTI_Pos);

    ADC1->CR |= ADC_CR_ADCAL;
    ADC2->CR |= ADC_CR_ADCAL;

    while ( (ADC1->CR & ADC_CR_ADCAL) || (ADC2->CR & ADC_CR_ADCAL) )
        ;

    ADC1->CR |= ADC_CR_ADEN;
    ADC2->CR |= ADC_CR_ADEN;

    /* Wait for ADC to be ready */
    while (!(ADC1->ISR & ADC_ISR_ADRDY) || !(ADC2->ISR & ADC_ISR_ADRDY) )
        ;

    /* External Trigger on TIM3_TRGO, left aligned data with 12 bit resolution, results by DMA, circular */
    ADC1->CFGR = (0x01 << ADC_CFGR_EXTEN_Pos)  | (0x04 << ADC_CFGR_EXTSEL_Pos) | (ADC_CFGR_ALIGN) | (0x00 << ADC_CFGR_RES_Pos)
               | ADC_CFGR_DMACFG | ADC_CFGR_DMAEN;
    ADC2->CFGR = (0x01 << ADC_CFGR_EXTEN_Pos)  | (0x04 << ADC_CFGR_EXTSEL_Pos) | (ADC_CFGR_ALIGN) | (0x00 << ADC_CFGR_RES_Pos)
               | ADC_CFGR_DMACFG | ADC_CFGR_DMAEN;

    /* Maximum sample time of 601.5 cycles for channel 3/channel 12. */
    ADC1->SMPR1 = 0x7 << ADC_SMPR1_SMP3_Pos;
    ADC2->SMPR2 = 0x7 << ADC_SMPR2_SMP12_Pos;

    /* Sample only channel 3/channel 12 in a regular sequence */
    ADC1->SQR1 = ( 3 << ADC_SQR1_SQ1_Pos) | (0 << ADC_SQR1_L_Pos);
    ADC2->SQR1 = (12 << ADC_SQR1_SQ1_Pos) | (0 << ADC_SQR1_L_Pos);

    /* No ADC interrupts but overrun (ADC1_2_IRQHandler): the DMA's come once per block */
    ADC1->IER = ADC_IER_OVRIE;
    ADC2->IER = ADC_IER_OVRIE;
    NVIC_SetPriority(ADC1_2_IRQn, AIOC_IRQ_PRIO_AUDIO);

    /* Recording DMA: ADC1 on DMA1 channel 1, ADC2 on DMA2 channel 1, the default mapping */
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();
    SYSCFG->CFGR1 &= ~SYSCFG_CFGR1_ADC24_DMA_RMP;
    NVIC_SetPriority(ADC1_DMA_IRQn, AIOC_IRQ_PRIO_AUDIO);
    NVIC_SetPriority(ADC2_DMA_IRQn, AIOC_IRQ_PRIO_AUDIO);
}

static void DAC_Init(void)
{
    __HAL_RCC_DAC1_CLK_ENABLE();

    /* Select TIM6 TRGO as trigger and enable DAC */
    DAC->CR = (0x0 << DAC_CR_TSEL1_Pos) | DAC_CR_TEN1 | DAC_CR_EN1;

    /* Output VDD/2 */
    DAC1->DHR12L1 = 32768;

    /* Playback DMA: DAC1 channel 1's request on DMA2 channel 3, the default mapping */
    __HAL_RCC_DMA2_CLK_ENABLE();
    SYSCFG->CFGR1 &= ~SYSCFG_CFGR1_TIM6DAC1Ch1_DMA_RMP;
    NVIC_SetPriority(DAC_DMA_IRQn, AIOC_IRQ_PRIO_AUDIO);

    /* The DAC's DMA underrun interrupt (enabled with playback) */
    NVIC_SetPriority(TIM6_DAC1_IRQn, AIOC_IRQ_PRIO_AUDIO);
    NVIC_ClearPendingIRQ(TIM6_DAC1_IRQn);
    NVIC_EnableIRQ(TIM6_DAC1_IRQn);
}

/* Set up the input for the gain, and return the ADC that reads it (Microphone_Start starts it) */
static ADC_TypeDef *RX_Config(usb_audio_rxgain_t rxGain)
{
    /* Disable OPAMPs */
    OPAMP1->CSR = 0x00;
    OPAMP2->CSR = 0x00;

    if (rxGain == USB_AUDIO_RXGAIN_1X) {
        /* Legacy mode that is compatible with pre-v1.2 hardware */
        OPAMP2->CSR = OPAMP_FOLLOWER_MODE | OPAMP_VREF_50VDDA | OPAMP_CSR_FORCEVP | OPAMP2_CSR_OPAMP2EN; /* 50% VDD for bias */

        /* ADC2 with direct hardware ADC input (no PGA in between) */
        return ADC2;
    } else {
        /* Initialize OPAMPs so that OPAMP1 is a PGA (non inverting) and OPAMP2 produces the correct DC-bias voltage according to OPAMP1 gain. */
        static const uint32_t pgaConfig[] = {
            [USB_AUDIO_RXGAIN_2X] = OPAMP_PGA_MODE | OPAMP_PGA_GAIN_2,          /* ADCin: 0.825V +/- 0.825V */
            [USB_AUDIO_RXGAIN_4X] = OPAMP_PGA_MODE | OPAMP_PGA_GAIN_4,          /* ADCin: 0.4125V +/- 0.4125V */
            [USB_AUDIO_RXGAIN_8X] = OPAMP_PGA_MODE | OPAMP_PGA_GAIN_8,          /* ADCin: 0.20625V +/- 0.20625V */
            [USB_AUDIO_RXGAIN_16X] = OPAMP_PGA_MODE | OPAMP_PGA_GAIN_16         /* ADCin: 0.103125V +/- 0.103125V */
        };

        static const uint32_t biasConfig[] = {
            [USB_AUDIO_RXGAIN_2X] = OPAMP_PGA_MODE | OPAMP_PGA_GAIN_8,          /* 3.3V * 3.3% * 8 = 0.8712V */
            [USB_AUDIO_RXGAIN_4X] = OPAMP_PGA_MODE | OPAMP_PGA_GAIN_4,          /* 3.3V * 3.3% * 4 = 0.4356V */
            [USB_AUDIO_RXGAIN_8X] = OPAMP_PGA_MODE | OPAMP_PGA_GAIN_2,          /* 3.3V * 3.3% * 2 = 0.2178V */
            [USB_AUDIO_RXGAIN_16X] = OPAMP_FOLLOWER_MODE                        /* 3.3V * 3.3% * 1 = 0.1089V */
        };

        /* Set up OPAMP1 PGA */
        OPAMP1->CSR = pgaConfig[rxGain] | OPAMP_NONINVERTINGINPUT_IO3 | OPAMP1_CSR_OPAMP1EN;

        /* Add a trimming offset adjustment to get closer to the required bias voltage reference of 1.65V/16 = 103mV.
         * Default setting is 3.3V * 3.3% = 109mV.
         * These return the factory trim values, because the CSR register (and thus the USERTRIM bit) is 0 */
        uint32_t trimmingOffsetP2 = (OPAMP2->CSR & OPAMP2_CSR_TRIMOFFSETP_Msk) >> OPAMP2_CSR_TRIMOFFSETP_Pos;
        uint32_t trimmingOffsetN2 = (OPAMP2->CSR & OPAMP2_CSR_TRIMOFFSETN_Msk) >> OPAMP2_CSR_TRIMOFFSETN_Pos;
        int8_t trimAdjust = 7; /* Found empirically. It looks like 1 step amounts to roughly 1mV input offset */

        /* Observe the register limits, do saturated arithmetic */
        trimmingOffsetP2 = (((int32_t) trimmingOffsetP2 + trimAdjust) <= 0x1F) ? trimmingOffsetP2 + trimAdjust : 0x1F;
        trimmingOffsetN2 = (((int32_t) trimmingOffsetN2 - trimAdjust) >= 0x00) ? trimmingOffsetN2 - trimAdjust : 0x00;

        /* Set up OPAMP2 bias voltage generation, disable the factory trimming */
        OPAMP2->CSR = biasConfig[rxGain] | OPAMP_VREF_3VDDA | OPAMP_CSR_FORCEVP | OPAMP2_CSR_OPAMP2EN | OPAMP_TRIMMING_USER;

        /* Load trimming values */
        OPAMP2->CSR |= (OPAMP2->CSR & ~(OPAMP2_CSR_TRIMOFFSETP_Msk | OPAMP2_CSR_TRIMOFFSETN_Msk)) |
                ((trimmingOffsetP2 << OPAMP2_CSR_TRIMOFFSETP_Pos) & OPAMP2_CSR_TRIMOFFSETP_Msk) |
                ((trimmingOffsetN2 << OPAMP2_CSR_TRIMOFFSETN_Pos) & OPAMP2_CSR_TRIMOFFSETN_Msk);

        /* ADC1 using PGA output as ADC input */
        return ADC1;
    }
}

static void TX_Config(usb_audio_txboost_t txBoost)
{
    /* Set PINA3 (ATTEN Hi-Z) in TX Boost mode, reset PINA3 (pull ATTEN low) in normal mode */
    if (txBoost == USB_AUDIO_TXBOOST_ON) {
        GPIOA->BSRR = GPIO_BSRR_BS_3;
    } else {
        GPIOA->BRR = GPIO_BRR_BR_3;
    }
}

static void Timeout_Timers_Init(void)
{
    uint32_t timerFreq = (HAL_RCC_GetHCLKFreq() == HAL_RCC_GetPCLK2Freq()) ? HAL_RCC_GetPCLK2Freq() : 2 * HAL_RCC_GetPCLK2Freq();
    uint32_t pttTimeout = (settingsRegMap[SETTINGS_REG_VPTT_TIMCTRL] & SETTINGS_REG_VPTT_TIMCTRL_TIMEOUT_MASK) >> SETTINGS_REG_VPTT_TIMCTRL_TIMEOUT_OFFS;
    uint32_t cosTimeout = (settingsRegMap[SETTINGS_REG_VCOS_TIMCTRL] & SETTINGS_REG_VCOS_TIMCTRL_TIMEOUT_MASK) >> SETTINGS_REG_VCOS_TIMCTRL_TIMEOUT_OFFS;

    __HAL_RCC_TIM16_CLK_ENABLE();
    __HAL_RCC_TIM17_CLK_ENABLE();

   /* TIM16 and TIM17 are timeout-counters for PTT and COS */
   TIM16->CR1 = TIM_CLOCKDIVISION_DIV1 | TIM_COUNTERMODE_UP;
   TIM16->PSC = timerFreq / 16000 - 1; /* 16 kHz counter */
   TIM16->CCR1 = pttTimeout - 1;
   TIM16->DIER = TIM_DIER_UIE | TIM_DIER_CC1IE;

   TIM17->CR1 = TIM_CLOCKDIVISION_DIV1 | TIM_COUNTERMODE_UP;
   TIM17->PSC = timerFreq / 16000 - 1; /* 16 kHz counter */
   TIM17->CCR1 = cosTimeout - 1;
   TIM17->DIER = TIM_DIER_UIE | TIM_DIER_CC1IE;

   NVIC_SetPriority(TIM16_IRQn, AIOC_IRQ_PRIO_AUDIO);
   NVIC_EnableIRQ(TIM16_IRQn);

   NVIC_SetPriority(TIM17_IRQn, AIOC_IRQ_PRIO_AUDIO);
   NVIC_EnableIRQ(TIM17_IRQn);
}

void USB_AudioInit(void)
{
    /* Cycle counter, for the RX equaliser's cost (SETTINGS_REG_INFO_RXEQCYC) and the block
     * interrupts' (SETTINGS_REG_INFO_RXCYC, SETTINGS_REG_INFO_TXCYC) */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    GPIO_Init();
    Timer_ADC_Init();
    Timer_DAC_Init();
    ADC_Init();
    DAC_Init();

    Timeout_Timers_Init();
}

void USB_AudioReset(void)
{
    /* No stream survives a USB bus reset or a new configuration, but tinyusb does not close
     * them (no close callback), so stop both here: a new stream then starts from scratch */
    Microphone_Stop();
    microphoneState = STATE_OFF;
    Speaker_Stop();
    speakerState = STATE_OFF;

    /* Update debug register */
    settingsRegMap[SETTINGS_REG_INFO_AUDIO0] &= ~(SETTINGS_REG_INFO_AUDIO0_RECSTATE_MASK | SETTINGS_REG_INFO_AUDIO0_PLAYSTATE_MASK);
}

void USB_AudioTask(void)
{
    /* Virtual COS changes from the TIM17 interrupt. Cleared before the state is read, so a
     * change in between is shown now and again on the next pass, never lost; two changes
     * between passes (far shorter than any COS timeout) show as the last one */
    if (cosVirtualPending) {
        cosVirtualPending = 0;
        COS_VirtualSetState(cosVirtualState);
    }

    /* RX equaliser status, from the main loop rather than at every block, to keep the
     * recording interrupt short. A read racing the interrupt can mix two blocks' values;
     * harmless here */
    settingsRegMap[SETTINGS_REG_INFO_RXEQ] = RxEq_Status(&rxEq);
    settingsRegMap[SETTINGS_REG_INFO_RXEQCYC] = RxEq_Cycles(&rxEq);
}

void USB_AudioGetSpeakerFeedbackStats(usb_audio_fbstats_t * status)
{
    *status = (usb_audio_fbstats_t) {
        .feedbackMin = speakerFeedbackMin,
        .feedbackMax = speakerFeedbackMax,
        .feedbackAvg = (uint32_t) (speakerFeedbackAvg >> 16)
    };
}

void USB_AudioGetSpeakerBufferStats(usb_audio_bufstats_t * status)
{
    *status = (usb_audio_bufstats_t) {
        .bufLevelMin = speakerBufferLvlMin,
        .bufLevelMax =  speakerBufferLvlMax,
        .bufLevelAvg = (uint16_t) (speakerBufferLvlAvg >> 16)
    };

}
