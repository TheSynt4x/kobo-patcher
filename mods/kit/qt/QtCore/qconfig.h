/* Qt's configure writes this header; Kobo's Qt 5.2.1 is a 32-bit ARM Linux build with the usual
 * features, so only what the headers need to compile against it is here. */
#define QT_POINTER_SIZE 4
#define QT_LARGEFILE_SUPPORT 64
#define QT_NO_DEBUG
#ifndef QT_BOOTSTRAPPED
#define QT_VISIBILITY_AVAILABLE
#endif
