/**
 * @file format.h
 * Byte layout of the `cwsym` v1 header, shared by the writer, the parser,
 * and the tests that patch files to provoke every rejection.
 *
 * Every field is little-endian at a fixed offset. Sections start on
 * 8-byte boundaries.
 */
#ifndef CWSYM_FORMAT_H
#define CWSYM_FORMAT_H

#define CWSYM_HDR_SIZE            128
#define CWSYM_HDR_MAGIC           0   /**< u8[6] "CWSYM\0" */
#define CWSYM_HDR_VERSION         6   /**< u16 */
#define CWSYM_HDR_RULES           8   /**< u16 */
#define CWSYM_HDR_ARCH            10  /**< u16 */
#define CWSYM_HDR_BUILD_ID        12  /**< u8[20] */
#define CWSYM_HDR_BUILD_ID_LEN    32  /**< u8, then u8[3] reserved */
#define CWSYM_HDR_COUNT           36  /**< u32 */
#define CWSYM_HDR_FLAGS           40  /**< u32 */
#define CWSYM_HDR_OFF_STARTS      44  /**< u32 */
#define CWSYM_HDR_OFF_SIZES       48  /**< u32 */
#define CWSYM_HDR_OFF_NAMES       52  /**< u32 */
#define CWSYM_HDR_OFF_STRINGS     56  /**< u32 */
#define CWSYM_HDR_LEN_STRINGS     60  /**< u32 */
#define CWSYM_HDR_OFF_DISP        64  /**< u32 */
#define CWSYM_HDR_LINE_COUNT      68  /**< u32 */
#define CWSYM_HDR_OFF_LINE_STARTS 72  /**< u32 */
#define CWSYM_HDR_OFF_LINE_LENS   76  /**< u32 */
#define CWSYM_HDR_OFF_LINE_FILES  80  /**< u32 */
#define CWSYM_HDR_OFF_LINE_LINES  84  /**< u32 */
#define CWSYM_HDR_SITE_COUNT      88  /**< u32 */
#define CWSYM_HDR_OFF_SITE_STARTS 92  /**< u32 */
#define CWSYM_HDR_OFF_SITE_LENS   96  /**< u32 */
#define CWSYM_HDR_OFF_SITE_CALLEES 100 /**< u32 */
#define CWSYM_HDR_OFF_SITE_FILES  104 /**< u32 */
#define CWSYM_HDR_OFF_SITE_LINES  108 /**< u32 */
#define CWSYM_HDR_OFF_SITE_PARENTS 112 /**< u32 */
#define CWSYM_HDR_OFF_DSTRINGS    116 /**< u32 */
#define CWSYM_HDR_LEN_DSTRINGS    120 /**< u32, then u8[4] reserved */

#define CWSYM_SECTION_ALIGN 8

#endif /* CWSYM_FORMAT_H */
