#ifndef _GEM_LOG_H_
#define _GEM_LOG_H_

#include <rte_log.h>

extern int gem_logtype;

#define PMD_LOG(level, fmt, args...) \
    rte_log(RTE_LOG_ ## level, gem_logtype, \
        "%s(): " fmt "\n", __func__, ##args)

#endif