#include "hlog.h"

#include <stdio.h>

#define BMU_VF_NUM 8u
#define BMU_L_POOL_NUM 4u

typedef struct BmuState {
    const char *instance_name;
    uint32_t map_table[BMU_VF_NUM][BMU_L_POOL_NUM];
    HLogQueryHandle map_table_query;
} BmuState;

static int32_t bmu_map_table_query(
    void *opaque, const HLogQueryRequest *request,
    HLogQueryWriteFn write, void *write_opaque)
{
    BmuState *bmu = opaque;
    uint32_t vf;
    uint32_t l_pool = 0;
    bool has_l_pool;
    int32_t rc;

    if (hlog_query_arg_u32(request, "vf", &vf) != HLOG_OK) {
        return HLOG_ERR_INVALID_ARGUMENT;
    }
    if (vf >= BMU_VF_NUM) {
        char message[96];
        (void)snprintf(
            message, sizeof(message),
            "invalid vf=%u; expected range 0..%u",
            vf, BMU_VF_NUM - 1);
        return hlog_query_set_error(HLOG_ERR_INVALID_ARGUMENT, message);
    }

    has_l_pool = hlog_query_arg_count(request, "l_pool") != 0;
    if (has_l_pool &&
        hlog_query_arg_u32(request, "l_pool", &l_pool) != HLOG_OK) {
        return HLOG_ERR_INVALID_ARGUMENT;
    }
    if (has_l_pool && l_pool >= BMU_L_POOL_NUM) {
        return hlog_query_set_error(
            HLOG_ERR_INVALID_ARGUMENT, "l_pool is out of range");
    }

    rc = hlog_query_printf(
        write, write_opaque,
        "schema=hlog-query-v1 target=bmu/%s/map-table status=ok\n",
        bmu->instance_name);
    if (rc != HLOG_OK) {
        return rc;
    }

    if (has_l_pool) {
        rc = hlog_query_printf(
            write, write_opaque,
            "record=map-entry vf=%u l_pool=%u vl_pool=%u\n",
            vf, l_pool, bmu->map_table[vf][l_pool]);
        if (rc != HLOG_OK) {
            return rc;
        }
        return hlog_query_printf(
            write, write_opaque,
            "summary record_count=1 invalid_count=0\n");
    }

    for (l_pool = 0; l_pool < BMU_L_POOL_NUM; l_pool++) {
        rc = hlog_query_printf(
            write, write_opaque,
            "record=map-entry vf=%u l_pool=%u vl_pool=%u\n",
            vf, l_pool, bmu->map_table[vf][l_pool]);
        if (rc != HLOG_OK) {
            return rc;
        }
    }
    return hlog_query_printf(
        write, write_opaque,
        "summary record_count=%u invalid_count=0\n",
        BMU_L_POOL_NUM);
}

int bmu_register_queries(BmuState *bmu)
{
    static const HLogQueryParamSpec params[] = {
        HLOG_QUERY_REQUIRED_U32("vf", "VF index, valid range 0..7"),
        HLOG_QUERY_OPTIONAL_U32("l_pool", "Optional logical pool filter"),
    };

    bmu->map_table_query = hlog_query_register(
        "bmu", bmu->instance_name, "map-table",
        "Show VF l_pool to vl_pool mappings", params,
        HLOG_ARRAY_SIZE(params), bmu_map_table_query, bmu);
    if (bmu->map_table_query == HLOG_QUERY_INVALID_HANDLE) {
        fprintf(stderr, "BMU query registration failed: %s\n",
                hlog_last_error());
        return -1;
    }
    return 0;
}

void bmu_unregister_queries(BmuState *bmu)
{
    if (bmu->map_table_query != HLOG_QUERY_INVALID_HANDLE) {
        (void)hlog_query_unregister(bmu->map_table_query);
        bmu->map_table_query = HLOG_QUERY_INVALID_HANDLE;
    }
}
