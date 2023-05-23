/*
 * Copyright (c) 2020 Arm Limited.
 * Copyright (c) 2021 Infineon Technologies AG
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>
#include "bootutil/bootutil_log.h"
#include "cy_security_cnt_platform.h"
#include "cy_service_app.h"
#include "sysflash/sysflash.h"
#include "cy_efuse.h"

#if defined MCUBOOT_HW_ROLLBACK_PROT

#ifdef NEED_FLASH_MAP
#undef NEED_FLASH_MAP
#endif

#define NEED_MAX_COUNTERS
#include "cy_flash_map.h"

#define TEST_BIT(var, pos) (0U != ((var) & (1UL << (pos))))

#define NV_COUNTER_EFUSE_OFFSET  0x60

static uint8_t get_array_member(uint32_t image_id)
{
    uint8_t start_bit_for_image_id = 0;
    uint32_t arr_size = sizeof(bits_per_cnt)/sizeof(bits_per_cnt[0]);

    for (uint32_t j = 0; j < image_id && j < arr_size; ++j) {
        start_bit_for_image_id += bits_per_cnt[j]; 
    }

    return start_bit_for_image_id;
}

static fih_uint counter_extract(uint32_t image_id, fih_uint nv_counter)
{
    uint32_t res = 0U;
    uint8_t start_bit_for_image_id = get_array_member(image_id);
    uint8_t bits_for_current_image = bits_per_cnt[image_id];

    while (TEST_BIT(fih_uint_decode(nv_counter), start_bit_for_image_id) && 
            bits_for_current_image != 0U)
    {
        ++res;
        ++start_bit_for_image_id;
        --bits_for_current_image;
    }

    return fih_uint_encode(res);
}

/**
 * Extracts security counter for the desired image from full NV
 * counter and converts it to integer value.
 * Efuse stores nv counter value as a consequent bits. This means
 * NV counter set to 5 in policy would be written as 0x1F.
 * Only one security counter is available in system. Maximum value is 32.
 * Since more than one image can be used, 32 bits of NV counter are divided into
 * number of images (it's on a user decision how many bits for each image).
 *
 * @param image_id          Index of the image (from 0)
 *
 * @param nv_counter        Full security counter to get specific efuse value for desired image
 * 
 * @param extracted_img_cnt Pointer to a variable, where extracted counter for the 'image_id'
 *                          would be stored
 *
 * @return                  FIH_FAILURE on failure, otherwise FIH_SUCCESS.
 *                      
 */

fih_int platform_security_counter_check_extract(uint32_t image_id, fih_uint nv_counter, fih_uint *extracted_img_cnt)
{
    fih_int fih_ret = FIH_FAILURE;
    uint32_t arr_size = sizeof(bits_per_cnt)/sizeof(bits_per_cnt[0]);
    
    if (image_id > (arr_size-1U)) {
        BOOT_LOG_ERR("Incorrect input parameter Image ID");
        FIH_RET(fih_ret);
    }

    uint8_t start_bit_for_image_id = get_array_member(image_id);
    uint8_t bits_for_current_image = bits_per_cnt[image_id];
    uint32_t bit_mask_to_check_others_images = 0U;

    /* Check if full NV-counter has any bits of others image_id */
        /* Set up the number of bits equal to bits_for_current_image */
    for (uint32_t j = 0; j < bits_for_current_image; ++j)
    {
        bit_mask_to_check_others_images <<= 1U;
        bit_mask_to_check_others_images |= 1U;
    }
        /* Move bit_mask_to_check_others_images at place for image_id */
    bit_mask_to_check_others_images <<= start_bit_for_image_id;

    /* Return an error if recieved full NV-counter has any bits of others image_id */
    if( !(~bit_mask_to_check_others_images & fih_uint_decode(nv_counter)) )
    {
        /* Extract number of set bits from full NV-counter in the upgrade image */
        *extracted_img_cnt = counter_extract(image_id, nv_counter);

        fih_ret = FIH_SUCCESS;
    }

    FIH_RET(fih_ret);
}

/**
 * Reads the security counter data from chip's EFUSEs and converts it to the actual value of 
 * security counter for each image.
 *
 * @param image_id          The image number for which you want to get a security counter,
 *                          saved in EFUSE
 * @param security_cnt      Pointer to a variable, where security counter value for the 'image_id'
 *                          would be stored
 *
 * @return                  FIH_SUCESS on success; FIH_FAILURE on failure.
 */
fih_int platform_security_counter_get(uint32_t image_id, fih_uint *security_cnt) {

    fih_int fih_ret = FIH_FAILURE;      
    uint32_t arr_size = sizeof(bits_per_cnt)/sizeof(bits_per_cnt[0]);

    if (image_id > (arr_size-1U)) {
        BOOT_LOG_ERR("Incorrect input parameter Image ID");
        FIH_RET(fih_ret);
    }

    cy_en_efuse_status_t efuse_stat = CY_EFUSE_ERR_UNC;
    uint32_t nv_counter = 0;
    fih_uint nv_counter_secure = (fih_uint)FIH_FAILURE;

    /* Init also enables Efuse block */
    efuse_stat = Cy_EFUSE_Init(EFUSE);

    if (efuse_stat == CY_EFUSE_SUCCESS) {

        efuse_stat = Cy_EFUSE_ReadWord(EFUSE, &nv_counter, NV_COUNTER_EFUSE_OFFSET);

        if (efuse_stat == CY_EFUSE_SUCCESS){
            /* Read value of counter from efuse twice to ensure value is not compromised */
            nv_counter_secure = fih_uint_encode(nv_counter);
            nv_counter = 0U;
            efuse_stat = Cy_EFUSE_ReadWord(EFUSE, &nv_counter, NV_COUNTER_EFUSE_OFFSET);
        }
        if (efuse_stat == CY_EFUSE_SUCCESS){

            if (fih_uint_eq(nv_counter_secure, fih_uint_encode(nv_counter))) {

                *security_cnt = counter_extract(image_id, nv_counter);

                fih_ret = FIH_SUCCESS;
            }
        }

        Cy_EFUSE_Disable(EFUSE);
        Cy_EFUSE_DeInit(EFUSE);
    }

    FIH_RET(fih_ret);
}

/**
 * Updates the stored value of a given security counter with a new
 * security counter value if the new one is greater.
 * Only one security counter is available in system. Maximum value is 32.
 * Since more than one image can be used, 32 bits of NV counter are divided into
 * number of images (it's on a user decision how many bits for each image).
 *
 * @param image_id          The image number for which you want to get a security counter,
 *                          saved in EFUSE (from 0)
 * @param img_security_cnt  Full new NV security counter
 * @param reprov_packet     Pointer to a reprovisioning packet containing new NV counter.
 * @return                  0 on success; nonzero on failure.
 */
int32_t platform_security_counter_update(uint32_t image_id, uint32_t img_security_cnt, uint8_t * reprov_packet)
{
    int32_t rc = -1;
    fih_int fih_rc = FIH_FAILURE;
    fih_uint efuse_img_counter = (fih_uint)FIH_FAILURE;
    fih_uint packet_img_counter = counter_extract(image_id, img_security_cnt);

    /* Read value of security counter stored in chips efuses.
     * Only one security counter is available in system. Maximum value is 32.
    */
    FIH_CALL(platform_security_counter_get, fih_rc, image_id, &efuse_img_counter);

    if (FIH_TRUE == fih_eq(fih_rc, FIH_SUCCESS)) {

        /* Compare the new image's security counter value against the
        * stored security counter value for that image index.
        */

        BOOT_LOG_DBG("image_id = %u, packet_img_counter = %u, efuse_img_counter = %u \n",
                image_id, packet_img_counter, fih_uint_decode(efuse_img_counter));

        if ((packet_img_counter > fih_uint_decode(efuse_img_counter)) &&
             (packet_img_counter <= MAX_SEC_COUNTER_VAL)) {

            BOOT_LOG_INF("service_app is called\n", __func__ );
            /* Attention: This function initiates system reset */
            call_service_app(reprov_packet);
            /* Runtime should never get here. Panic statement added to secure
             * sutiation when hacker initiates skip of call_service_app function. 
            */
            FIH_PANIC;
        }
        else {
            rc = 0;
        }
    }

return rc;
}

#endif /* defined MCUBOOT_HW_ROLLBACK_PROT */
