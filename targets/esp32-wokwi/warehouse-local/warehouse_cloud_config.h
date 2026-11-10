#ifndef WAREHOUSE_CLOUD_CONFIG_H
#define WAREHOUSE_CLOUD_CONFIG_H
// Run tools/warehouse_cloud.py configure with your public Firebase identifiers.
// Device passwords are entered at runtime, never stored in this header.
#ifndef WAREHOUSE_CLOUD_ENABLED
#define WAREHOUSE_CLOUD_ENABLED 0
#endif
static const char WM_FIREBASE_API_KEY[] = "";
static const char WM_FIREBASE_DATABASE_URL[] = "";
static const char WM_DEVICE_UID[] = "";
static const char WM_DEVICE_ID[] = "warehouse-01";
#endif
