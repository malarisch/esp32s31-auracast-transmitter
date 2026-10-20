# SPDX-License-Identifier: Apache-2.0
# Pure C DSP building blocks: builds as an ESP-IDF component or as a host library for tests.
set(AUDIO_DSP_SRCS spsc_ring.c resampler.c decimator.c big_airtime.c drift_ctrl.c rate_check.c tx_pacing.c)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${AUDIO_DSP_SRCS}
                           INCLUDE_DIRS include)
else()
    add_library(audio_dsp STATIC ${AUDIO_DSP_SRCS})
    target_include_directories(audio_dsp PUBLIC include)
    target_link_libraries(audio_dsp PUBLIC m)
endif()
