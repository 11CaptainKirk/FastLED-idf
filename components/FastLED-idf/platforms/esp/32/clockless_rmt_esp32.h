/*
 * Integration into FastLED ClocklessController
 * Copyright (c) 2018 Samuel Z. Guyer
 * Copyright (c) 2017 Thomas Basler
 * Copyright (c) 2017 Martin F. Falatic
 *
 * ESP32 support is provided using the RMT peripheral device -- a unit
 * on the chip designed specifically for generating (and receiving)
 * precisely-timed digital signals. Nominally for use in infrared
 * remote controls, we use it to generate the signals for clockless
 * LED strips. The main advantage of using the RMT device is that,
 * once programmed, it generates the signal asynchronously, allowing
 * the CPU to continue executing other code. It is also not vulnerable
 * to interrupts or other timing problems that could disrupt the signal.
 *
 * The implementation strategy is borrowed from previous work and from
 * the RMT support built into the ESP32 IDF. The RMT device has 8
 * channels, which can be programmed independently to send sequences
 * of high/low bits. Memory for each channel is limited, however, so
 * in order to send a long sequence of bits, we need to continuously
 * refill the buffer until all the data is sent. To do this, we fill
 * half the buffer and then set an interrupt to go off when that half
 * is sent. Then we refill that half while the second half is being
 * sent. This strategy effectively overlaps computation (by the CPU)
 * and communication (by the RMT).
 *
 * Since the RMT device only has 8 channels, we need a strategy to
 * allow more than 8 LED controllers. Our driver assigns controllers
 * to channels on the fly, queuing up controllers as necessary until a
 * channel is free. The main showPixels routine just fires off the
 * first 8 controllers; the interrupt handler starts new controllers
 * asynchronously as previous ones finish. So, for example, it can
 * send the data for 8 controllers simultaneously, but 16 controllers
 * would take approximately twice as much time.
 *
 * There is a #define that allows a program to control the total
 * number of channels that the driver is allowed to use. It defaults
 * to 8 -- use all the channels. Setting it to 1, for example, results
 * in fully serial output:
 *
 *     #define FASTLED_RMT_MAX_CHANNELS 1
 *
 * OTHER RMT APPLICATIONS
 *
 * The default FastLED driver takes over control of the RMT interrupt
 * handler, making it hard to use the RMT device for other
 * (non-FastLED) purposes. You can change it's behavior to use the ESP
 * core driver instead, allowing other RMT applications to
 * co-exist. To switch to this mode, add the following directive
 * before you include FastLED.h:
 *
 *      #define FASTLED_RMT_BUILTIN_DRIVER
 *
 * There may be a performance penalty for using this mode. We need to
 * compute the RMT signal for the entire LED strip ahead of time,
 * rather than overlapping it with communication. We also need a large
 * buffer to hold the signal specification. Each bit of pixel data is
 * represented by a 32-bit pulse specification, so it is a 32X blow-up
 * in memory use.
 *
 *
 * Based on public domain code created 19 Nov 2016 by Chris Osborn <fozztexx@fozztexx.com>
 * http://insentricity.com *
 *
 */
/*
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

FASTLED_NAMESPACE_BEGIN

#ifdef __cplusplus
extern "C" {
#endif

#include "esp32-hal.h"
#include "esp_intr_alloc.h"
#include "driver/gpio.h"
#include "driver/rmt.h"
#include "driver/periph_ctrl.h"
#include "freertos/semphr.h"
#include "soc/rmt_struct.h"

#include "esp_log.h"

// needed to work around issue with driver problem in 4.1 and master around 2020
#include "esp_idf_version.h"

#ifdef __cplusplus
}
#endif

__attribute__ ((always_inline)) inline static uint32_t __clock_cycles() {
  uint32_t cyc;
  __asm__ __volatile__ ("rsr %0,ccount":"=a" (cyc));
  return cyc;
}

#define FASTLED_HAS_CLOCKLESS 1
#define NUM_COLOR_CHANNELS 3

// -- Set to true to print debugging information about timing
//    Useful for finding out if timing is being messed up by other things
//    on the processor (WiFi, for example)
#ifndef FASTLED_RMT_SHOW_TIMER
#define FASTLED_RMT_SHOW_TIMER false
#endif

// -- Configuration constants
#define DIVIDER             2 /* 4, 8 still seem to work, but timings become marginal */
#define MAX_PULSES         64 /* A channel has a 64 "pulse" buffer */
#define PULSES_PER_FILL    24 /* One pixel's worth of pulses */

// -- Convert ESP32 CPU cycles to RMT device cycles, taking into account the divider
#define F_CPU_RMT                   (  80000000L)
#define RMT_CYCLES_PER_SEC          (F_CPU_RMT/DIVIDER)
#define RMT_CYCLES_PER_ESP_CYCLE    (F_CPU / RMT_CYCLES_PER_SEC)
#define ESP_TO_RMT_CYCLES(n)        ((n) / (RMT_CYCLES_PER_ESP_CYCLE))

// -- Number of cycles to signal the strip to latch
#define NS_PER_CYCLE                ( 1000000000L / RMT_CYCLES_PER_SEC )
#define NS_TO_CYCLES(n)             ( (n) / NS_PER_CYCLE )
#define RMT_RESET_DURATION          NS_TO_CYCLES(50000)

// -- Core or custom driver --- 'builtin' is the core driver which is supposedly slower
#ifndef FASTLED_RMT_BUILTIN_DRIVER

// NOTE!
// there is an upstream issue with using the custom driver. This is in https://github.com/espressif/esp-idf/issues/5476
// In this, it states that in order to use one of the functions, the upstream must be modified.
// 
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4,1,0)
#define FASTLED_RMT_BUILTIN_DRIVER true
#else
#define FASTLED_RMT_BUILTIN_DRIVER false
#endif

#endif

// -- Max number of controllers we can support
#ifndef FASTLED_RMT_MAX_CONTROLLERS
#define FASTLED_RMT_MAX_CONTROLLERS 32
#endif

// -- Number of RMT channels to use (up to 8)
//    Redefine this value to 1 to force serial output
#ifndef FASTLED_RMT_MAX_CHANNELS
#define FASTLED_RMT_MAX_CHANNELS 8
#endif

// -- Array of all controllers
static CLEDController * gControllers[FASTLED_RMT_MAX_CONTROLLERS];

// -- Current set of active controllers, indexed by the RMT
//    channel assigned to them.
static CLEDController * gOnChannel[FASTLED_RMT_MAX_CHANNELS];

static int gNumControllers = 0;
static int gNumStarted = 0;
static int gNumDone = 0;
static int gNext = 0;

static intr_handle_t gRMT_intr_handle = NULL;

// -- Global semaphore for the whole show process
//    Semaphore is not given until all data has been sent
static xSemaphoreHandle gTX_sem = NULL;

static bool gInitialized = false;

template <int DATA_PIN, int T1, int T2, int T3, EOrder RGB_ORDER = RGB, int XTRA0 = 0, bool FLIP = false, int WAIT_TIME = 5>
class ClocklessController : public CPixelLEDController<RGB_ORDER>
{
    // -- RMT has 8 channels, numbered 0 to 7
    rmt_channel_t  mRMT_channel;

    // -- Store the GPIO pin
    gpio_num_t     mPin;

    // -- This instantiation forces a check on the pin choice
    FastPin<DATA_PIN> mFastPin;

    // -- Timing values for zero and one bits, derived from T1, T2, and T3
    rmt_item32_t   mZero;
    rmt_item32_t   mOne;

    // -- Save the pixel controller
    PixelController<RGB_ORDER> * mPixels;
    int            mCurColor;
    uint16_t       mCurPulse;
    volatile uint32_t * mRMT_mem_ptr;

    // -- Buffer to hold all of the pulses. For the version that uses
    //    the RMT driver built into the ESP core.
    rmt_item32_t * mBuffer;
    uint16_t       mBufferSize;

public:

    void init()
    {
        // -- Allocate space to save the pixel controller
        //    during parallel output
        mPixels = (PixelController<RGB_ORDER> *) malloc(sizeof(PixelController<RGB_ORDER>));
        
        // -- Precompute rmt items corresponding to a zero bit and a one bit
        //    according to the timing values given in the template instantiation
        // T1H
        mOne.level0 = 1;
        mOne.duration0 = ESP_TO_RMT_CYCLES(T1+T2); // TO_RMT_CYCLES(T1+T2);
        // T1L
        mOne.level1 = 0;
        mOne.duration1 = ESP_TO_RMT_CYCLES(T3); // TO_RMT_CYCLES(T3);

        // T0H
        mZero.level0 = 1;
        mZero.duration0 = ESP_TO_RMT_CYCLES(T1); // TO_RMT_CYCLES(T1);
        // T0L
        mZero.level1 = 0;
        mZero.duration1 = ESP_TO_RMT_CYCLES(T2+T3); // TO_RMT_CYCLES(T2 + T3);

        gControllers[gNumControllers] = this;
        gNumControllers++;

        mPin = gpio_num_t(DATA_PIN);
    }

    virtual uint16_t getMaxRefreshRate() const { return 400; }

protected:

    void initRMT()
    {
        for (int i = 0; i < FASTLED_RMT_MAX_CHANNELS; i++) {
            gOnChannel[i] = NULL;

            // -- RMT configuration for transmission
            rmt_config_t rmt_tx;
            rmt_tx.channel = rmt_channel_t(i);
            rmt_tx.rmt_mode = RMT_MODE_TX;
            rmt_tx.gpio_num = mPin;  // The particular pin will be assigned later
            rmt_tx.mem_block_num = 1;
            rmt_tx.clk_div = DIVIDER;
            rmt_tx.tx_config.loop_en = false;
            rmt_tx.tx_config.carrier_level = RMT_CARRIER_LEVEL_LOW;
            rmt_tx.tx_config.carrier_en = false;
            rmt_tx.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
            rmt_tx.tx_config.idle_output_en = true;
                
            // -- Apply the configuration
            rmt_config(&rmt_tx);

            if (FASTLED_RMT_BUILTIN_DRIVER) {
                rmt_driver_install(rmt_channel_t(i), 0, 0);
            } else {
                // -- Set up the RMT to send 1 pixel of the pulse buffer and then
                //    generate an interrupt. When we get this interrupt we
                //    fill the other part in preparation (kind of like double-buffering)
                rmt_set_tx_thr_intr_en(rmt_channel_t(i), true, PULSES_PER_FILL);
            }
        }

        // -- Create a semaphore to block execution until all the controllers are done
        if (gTX_sem == NULL) {
            gTX_sem = xSemaphoreCreateBinary();
            xSemaphoreGive(gTX_sem);
        }
                

    // this was crashing in 4.0. I am hoping that registering the IRS through rmt_isr_register does the right thing.

        if ( ! FASTLED_RMT_BUILTIN_DRIVER) {
            // -- Allocate the interrupt if we have not done so yet. This
            //    interrupt handler must work for all different kinds of
            //    strips, so it delegates to the refill function for each
            //    specific instantiation of ClocklessController.
            if (gRMT_intr_handle == NULL)
                esp_intr_alloc(ETS_RMT_INTR_SOURCE, ESP_INTR_FLAG_LEVEL3, interruptHandler, 0, &gRMT_intr_handle);
        }

        gInitialized = true;
    }

    // -- Show pixels
    //    This is the main entry point for the controller.
    virtual void IRAM_ATTR showPixels(PixelController<RGB_ORDER> & pixels)
    {
        if (gNumStarted == 0) {
            // -- First controller: make sure everything is set up
            // -- Only need to do this once
            if ( ! gInitialized) {
                initRMT();
            }
            xSemaphoreTake(gTX_sem, portMAX_DELAY);
        }

        if (FASTLED_RMT_BUILTIN_DRIVER)
            convertAllPixelData(pixels);
        else {
            // -- Initialize the local state, save a pointer to the pixel
            //    data. We need to make a copy because pixels is a local
            //    variable in the calling function, and this data structure
            //    needs to outlive this call to showPixels.
            (*mPixels) = pixels;
        }

        // -- Keep track of the number of strips we've seen
        gNumStarted++;

        // -- The last call to showPixels is the one responsible for doing
        //    all of the actual work
        if (gNumStarted == gNumControllers) {
            gNext = 0;

            // -- First, fill all the available channels
            int channel = 0;
            while (channel < FASTLED_RMT_MAX_CHANNELS && gNext < gNumControllers) {
                startNext(channel);
                channel++;
            }

            // -- Start them all
            for (int i = 0; i < channel; i++) {
                ClocklessController * pController = static_cast<ClocklessController*>(gControllers[i]);
                rmt_tx_start(pController->mRMT_channel, true);
            }

            // -- Wait here while the rest of the data is sent. The interrupt handler
            //    will keep refilling the RMT buffers until it is all sent; then it
            //    gives the semaphore back.
            xSemaphoreTake(gTX_sem, portMAX_DELAY);
            xSemaphoreGive(gTX_sem);

            // -- Reset the counters
            gNumStarted = 0;
            gNumDone = 0;
            gNext = 0;
        }
    }

    // -- Convert all pixels to RMT pulses
    //    This function is only used when the user chooses to use the
    //    built-in RMT driver, which needs all of the RMT pulses
    //    up-front.
    void convertAllPixelData(PixelController<RGB_ORDER> & pixels)
    {
        // -- Compute the pulse values for the whole strip at once.
        //    Requires a large buffer
        mBufferSize = pixels.size() * 3 * 8;

        if (mBuffer == NULL) {
            mBuffer = (rmt_item32_t *) calloc( mBufferSize, sizeof(rmt_item32_t));
        }

        // -- Cycle through the R,G, and B values in the right order,
        //    storing the pulses in the big buffer
        mCurPulse = 0;

        uint32_t byteval;
        while (pixels.has(1)) {
            byteval = pixels.loadAndScale0();
            convertByte(byteval);
            byteval = pixels.loadAndScale1();
            convertByte(byteval);
            byteval = pixels.loadAndScale2();
            convertByte(byteval);
            pixels.advanceData();
            pixels.stepDithering();
        }

        mBuffer[mCurPulse-1].duration1 = RMT_RESET_DURATION;
        assert(mCurPulse == mBufferSize);
    }

    void convertByte(uint32_t byteval)
    {
        // -- Write one byte's worth of RMT pulses to the big buffer
        byteval <<= 24;
        for (register uint32_t j = 0; j < 8; j++) {
            mBuffer[mCurPulse] = (byteval & 0x80000000L) ? mOne : mZero;
            byteval <<= 1;
            mCurPulse++;
        }
    }

    // -- Start up the next controller
    //    This method is static so that it can dispatch to the
    //    appropriate startOnChannel method of the given controller.
    static void IRAM_ATTR startNext(int channel)
    {
        if (gNext < gNumControllers) {
            ClocklessController * pController = static_cast<ClocklessController*>(gControllers[gNext]);
            pController->startOnChannel(channel);
            gNext++;
        }
    }

    // -- Start this controller on the given channel
    //    This function just initiates the RMT write; it does not wait
    //    for it to finish.
    void IRAM_ATTR startOnChannel(int channel)
    {
        // -- Assign this channel and configure the RMT
        mRMT_channel = rmt_channel_t(channel);

        // -- Store a reference to this controller, so we can get it
        //    inside the interrupt handler
        gOnChannel[channel] = this;

        // -- Assign the pin to this channel
        rmt_set_pin(mRMT_channel, RMT_MODE_TX, mPin);

        if (FASTLED_RMT_BUILTIN_DRIVER) {
            // -- Use the built-in RMT driver to send all the data in one shot
            rmt_register_tx_end_callback(doneOnChannel, 0);
            rmt_write_items(mRMT_channel, mBuffer, mBufferSize, false);
        } else {
            // -- Use our custom driver to send the data incrementally

            // -- Initialize the counters that keep track of where we are in
            //    the pixel data.
            mRMT_mem_ptr = & (RMTMEM.chan[mRMT_channel].data32[0].val);
            mCurPulse = 0;
            mCurColor = 0;

            // -- Store 2 pixels worth of data (two "buffers" full)
            fillNext();
            fillNext();

            // -- Turn on the interrupts
            rmt_set_tx_intr_en(mRMT_channel, true);
        }
    }

    // -- A controller is done 
    //    This function is called when a controller finishes writing
    //    its data. It is called either by the custom interrupt
    //    handler (below), or as a callback from the built-in
    //    interrupt handler. It is static because we don't know which
    //    controller is done until we look it up.
    static void IRAM_ATTR doneOnChannel(rmt_channel_t channel, void * arg)
    {
        ClocklessController * controller = static_cast<ClocklessController*>(gOnChannel[channel]);
        portBASE_TYPE HPTaskAwoken = 0;

        // -- Turn off output on the pin
        gpio_matrix_out(controller->mPin, 0x100, 0, 0);

        gOnChannel[channel] = NULL;
        gNumDone++;

        if (gNumDone == gNumControllers) {
            // -- If this is the last controller, signal that we are all done
            xSemaphoreGiveFromISR(gTX_sem, &HPTaskAwoken);
            if(HPTaskAwoken == pdTRUE) portYIELD_FROM_ISR();
        } else {
            // -- Otherwise, if there are still controllers waiting, then
            //    start the next one on this channel
            if (gNext < gNumControllers) {
                startNext(channel);
                // -- Start the RMT TX operation
                //    (I'm not sure if this is necessary here)
                rmt_tx_start(controller->mRMT_channel, true);
            }
        }
    }
    
    // -- Custom interrupt handler
    //    This interrupt handler handles two cases: a controller is
    //    done writing its data, or a controller needs to fill the
    //    next half of the RMT buffer with data.
    static void IRAM_ATTR interruptHandler(void *arg)
    {
        // -- The basic structure of this code is borrowed from the
        //    interrupt handler in esp-idf/components/driver/rmt.c
        uint32_t intr_st = RMT.int_st.val;
        uint8_t channel;

        for (channel = 0; channel < FASTLED_RMT_MAX_CHANNELS; channel++) {
            int tx_done_bit = channel * 3;
            int tx_next_bit = channel + 24;

            if (gOnChannel[channel] != NULL) {

                // -- More to send on this channel
                if (intr_st & BIT(tx_next_bit)) {
                    RMT.int_clr.val |= BIT(tx_next_bit);
                    
                    // -- Refill the half of the buffer that we just finished,
                    //    allowing the other half to proceed.
                    ClocklessController * controller = static_cast<ClocklessController*>(gOnChannel[channel]);
                    controller->fillNext();
                } else {
                    // -- Transmission is complete on this channel
                    if (intr_st & BIT(tx_done_bit)) {
                        RMT.int_clr.val |= BIT(tx_done_bit);
                        doneOnChannel(rmt_channel_t(channel), 0);
                    }
                }
            }
        }
    }

    // -- Fill RMT buffer
    //    Puts one pixel's worth of data into the next 24 slots in the RMT memory
    void IRAM_ATTR fillNext()
    {
        if (mPixels->has(1)) {
            // bb compiler complains
            //uint32_t t1 = __clock_cycles();
            
            uint32_t one_val = mOne.val;
            uint32_t zero_val = mZero.val;

            // -- Get a pixel's worth of data
            uint8_t byte0 = mPixels->loadAndScale0();
            uint8_t byte1 = mPixels->loadAndScale1();
            uint8_t byte2 = mPixels->loadAndScale2();
            mPixels->advanceData();
            mPixels->stepDithering();

            // -- Fill 24 slots in the RMT memory
            register uint32_t pixel = byte0 << 24 | byte1 << 16 | byte2 << 8;

            // -- Use locals for speed
            volatile register uint32_t * pItem =  mRMT_mem_ptr;
            register uint16_t curPulse = mCurPulse;
            
            // Shift bits out, MSB first, setting RMTMEM.chan[n].data32[x] to the 
            // rmt_item32_t value corresponding to the buffered bit value
            for (register uint32_t j = 0; j < 24; j++) {
                uint32_t val = (pixel & 0x80000000L) ? one_val : zero_val;
                *pItem++ = val;
                // Replaces: RMTMEM.chan[mRMT_channel].data32[mCurPulse].val = val;

                pixel <<= 1;
                curPulse++;

                if (curPulse == MAX_PULSES) {
                    pItem = & (RMTMEM.chan[mRMT_channel].data32[0].val);
                    curPulse = 0;
                }
            }

            // -- Store the new values back into the object
            mCurPulse = curPulse;
            mRMT_mem_ptr = pItem;
        } else {
            // -- No more data; signal to the RMT we are done
            for (uint32_t j = 0; j < 8; j++) {
                * mRMT_mem_ptr++ = 0;
            }
        }   
    }



    // NO LONGER USED
    // -- Fill the RMT buffer
    //    This function fills the next 32 slots in the RMT write
    //    buffer with pixel data. It also handles the case where the
    //    pixel data is exhausted, so we need to fill the RMT buffer
    //    with zeros to signal that it's done.
    virtual void IRAM_ATTR fillHalfRMTBuffer()
    {
        uint32_t one_val = mOne.val;
        uint32_t zero_val = mZero.val;

        // -- Convert (up to) 32 bits of the raw pixel data into
        //    into RMT pulses that encode the zeros and ones.
        int pulses = 0;
        register uint32_t byteval;
        while (pulses < 32 && mPixels->has(1)) {
            // -- Get one byte
            // -- Cycle through the color channels
            switch (mCurColor) {
            case 0: 
                byteval = mPixels->loadAndScale0();
                break;
            case 1: 
                byteval = mPixels->loadAndScale1();
                break;
            case 2: 
                byteval = mPixels->loadAndScale2();
                mPixels->advanceData();
                mPixels->stepDithering();
                break;
            default:
                // -- This is bad!
                byteval = 0;
            }

            mCurColor++;
            if (mCurColor == NUM_COLOR_CHANNELS) mCurColor = 0;
        
            byteval <<= 24;
            // Shift bits out, MSB first, setting RMTMEM.chan[n].data32[x] to the 
            // rmt_item32_t value corresponding to the buffered bit value
            for (register uint32_t j = 0; j < 8; j++) {
                uint32_t val = (byteval & 0x80000000L) ? one_val : zero_val;
                * mRMT_mem_ptr++ = val;
                // Replaces: RMTMEM.chan[mRMT_channel].data32[mCurPulse].val = val;
                byteval <<= 1;
                mCurPulse++;
            }
            pulses += 8;
        }

        // -- When we reach the end of the pixel data, fill the rest of the
        //    RMT buffer with 0's, which signals to the device that we're done.
        if ( ! mPixels->has(1) ) {
            while (pulses < 32) {
                * mRMT_mem_ptr++ = 0;
                // Replaces: RMTMEM.chan[mRMT_channel].data32[mCurPulse].val = 0;
                mCurPulse++;
                pulses++;
            }
        }
        
        // -- When we have filled the back half the buffer, reset the position to the first half
        if (mCurPulse == MAX_PULSES) {
            mRMT_mem_ptr = & (RMTMEM.chan[mRMT_channel].data32[0].val);
            mCurPulse = 0;
        }
    }
};

FASTLED_NAMESPACE_END
