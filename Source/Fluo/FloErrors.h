/*****************************************************************
|
|   Fluo - Error Constants
|
|   (c) 2002-2006 Gilles Boccon-Gibod
|   Author: Gilles Boccon-Gibod (bok@bok.net)
|
 ****************************************************************/
/** @file
 * Fluo Result and Error codes
 */

#ifndef _FLO_ERRORS_H_
#define _FLO_ERRORS_H_

/*----------------------------------------------------------------------
|    includes
+---------------------------------------------------------------------*/
#include "Atomix.h"

/*----------------------------------------------------------------------
|    error codes
+---------------------------------------------------------------------*/
#define FLO_SUCCESS      0
#define FLO_FAILURE      (-1)

#define FLO_FAILED(result)       ((result) != FLO_SUCCESS)
#define FLO_SUCCEEDED(result)    ((result) == FLO_SUCCESS)

#define FLO_ERROR_IS_FATAL(result) (             \
    (result) != FLO_ERROR_NOT_ENOUGH_DATA &&     \
    (result) != FLO_ERROR_SAMPLES_SKIPPED &&     \
    (result) != FLO_ERROR_NO_MORE_SAMPLES &&     \
    (result) != FLO_ERROR_CORRUPTED_BITSTREAM && \
    (result) != FLO_ERROR_FRAME_SKIPPED          \
)

/* Decoder errors */
#define FLO_ERROR_BASE_DECODER        (-10000)

/* BitStream errors */
#define FLO_ERROR_BASE_BITSTREAM      (-10100)

/*----------------------------------------------------------------------
|    import some Atomix error codes
+---------------------------------------------------------------------*/
#define FLO_ERROR_OUT_OF_MEMORY      ATX_ERROR_OUT_OF_MEMORY
#define FLO_ERROR_INVALID_PARAMETERS ATX_ERROR_INVALID_PARAMETERS

#endif /* _FLO_ERRORS_H_ */
