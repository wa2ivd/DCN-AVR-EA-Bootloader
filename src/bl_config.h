#ifndef BL_CONFIG_H
#define BL_CONFIG_H

/*
 * Master config selector for the bootloader build.
 *
 * Set PRODUCT on the make command line (make PRODUCT=SPS-1). The Makefile
 * adds products/<PRODUCT>/ to the include path so this picks up the right
 * product header below. To add a new product, copy products/template/
 * and edit the constants in it.
 */

#include "bl_product_config.h"

/* Required keys every product config must define. */
#ifndef BL_PRODUCT_NAME
#  error "bl_product_config.h must define BL_PRODUCT_NAME"
#endif
#ifndef BL_DCN_ADDRESS
#  error "bl_product_config.h must define BL_DCN_ADDRESS"
#endif
#ifndef BL_BAUD
#  error "bl_product_config.h must define BL_BAUD"
#endif
#ifndef BL_APP_START
#  error "bl_product_config.h must define BL_APP_START"
#endif
#ifndef BL_FLASH_END
#  error "bl_product_config.h must define BL_FLASH_END"
#endif
#ifndef BL_PAGE_SIZE
#  error "bl_product_config.h must define BL_PAGE_SIZE"
#endif
#ifndef BL_APP_CRC_ADDR
#  error "bl_product_config.h must define BL_APP_CRC_ADDR (top-of-image CRC trailer)"
#endif

/* BL_VERSION is overridable but defaulted here so a product config can stay terse. */
#ifndef BL_VERSION
#  define BL_VERSION 0x0100u
#endif

#endif /* BL_CONFIG_H */
