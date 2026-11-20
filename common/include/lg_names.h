#ifndef LG_NAMES_H
#define LG_NAMES_H

/* Names, device paths and priorities shared by all processes (README 4.2, 7.3). */

/* name_attach() names (registered under /dev/name/local/). */
#define LG_NAME_CONTROLLER "lg/controller"
#define LG_NAME_GATE "lg/gate"
#define LG_NAME_SUPERVISOR "lg/supervisor"
#define LG_NAME_HEALTH "lg/health"
#define LG_NAME_SCHEDULE "lg/schedule"
#define LG_NAME_BRIDGE "lg/bridge"
#define LG_NAME_PLANT "lg/plant"
#define LG_NAME_SENSOR_FMT "lg/sensor/%s/%s" /* direction, id */

/* Resource manager paths. */
#define LG_DEV_GATE "/dev/lg/gate"
#define LG_DEV_PLANT "/dev/lg/plant"
#define LG_DEV_SENSOR_DIR "/dev/lg/sensor"
#define LG_DEV_SENSOR_FMT "/dev/lg/sensor/%s/%s" /* direction, id */

/* Direction and sensor path segments, indexed by lg_dir_t and lg_sensor_id_t. */
#define LG_DIR_NAME_WEST "west"
#define LG_DIR_NAME_EAST "east"
#define LG_SENSOR_NAME_A "a"
#define LG_SENSOR_NAME_B "b"
#define LG_SENSOR_NAME_ISLAND "island"

/* SCHED_FIFO priorities. */
#define LG_PRIO_SUPERVISOR 55
#define LG_PRIO_CONTROLLER 50
#define LG_PRIO_GATE 48
#define LG_PRIO_SENSOR 45
#define LG_PRIO_PLANT 40
#define LG_PRIO_HEALTH 20
#define LG_PRIO_SCHEDULE 15
#define LG_PRIO_BRIDGE 10
#define LG_PRIO_CONNECTOR 10 /* helper threads that may block while (re)connecting */
#define LG_PRIO_METER 42     /* lg-meter: evaluation only, samples the gate and the plant truth */

/* Installed locations on the target. */
#define LG_ETC_DIR "/etc/lintasguard"

#endif /* LG_NAMES_H */
