use hlog::{
    create_module_logger,
    ldebug,
    lerror,
    lhexdump,
    linfo,
    named,
    register_query,
    LogLevel,
    QueryError,
    QueryRegistration,
    SharedLogger,
};
use std::{fmt::Write as _, sync::{Arc, RwLock}};

const BMU_VF_NUM: u32 = 8;

#[derive(Clone, Copy)]
struct VfId(u32);

impl TryFrom<u32> for VfId {
    type Error = QueryError;

    fn try_from(value: u32) -> Result<Self, Self::Error> {
        if value < BMU_VF_NUM {
            Ok(Self(value))
        } else {
            Err(QueryError::invalid_argument(format!(
                "invalid vf={value}; expected range 0..{}",
                BMU_VF_NUM - 1
            )))
        }
    }
}

struct BmuState {
    map_table: [[u32; 4]; BMU_VF_NUM as usize],
}

struct Bmu {
    logger: SharedLogger,
    state: Arc<RwLock<BmuState>>,
    _map_table_query: QueryRegistration,
}

impl Bmu {
    fn new(instance: &str) -> Self {
        let state = Arc::new(RwLock::new(BmuState {
            map_table: [[0; 4]; BMU_VF_NUM as usize],
        }));
        let query_state = state.clone();
        let query_instance = instance.to_owned();
        let map_table_query = register_query("bmu", instance, "map-table")
            .description("Show VF l_pool to vl_pool mappings")
            .required_u32("vf", "VF index, valid range 0..7")
            .optional_u32("l_pool", "Optional logical pool filter")
            .register(move |args, output| {
                let vf = VfId::try_from(args.required_u32("vf")?)?;
                let l_pool = args.optional_u32("l_pool")?;
                let map_table = {
                    let state = query_state
                        .read()
                        .map_err(|_| QueryError::internal("BMU state lock is poisoned"))?;
                    state.map_table[vf.0 as usize]
                };

                if l_pool.is_some_and(|value| value as usize >= map_table.len()) {
                    return Err(QueryError::invalid_argument(
                        "l_pool is out of range",
                    ));
                }

                writeln!(
                    output,
                    "schema=hlog-query-v1 target=bmu/{}/map-table status=ok",
                    query_instance,
                )
                .map_err(|_| QueryError::internal("failed to write query output"))?;
                if let Some(l_pool) = l_pool {
                    let value = map_table[l_pool as usize];
                    writeln!(
                        output,
                        "record=map-entry vf={} l_pool={} vl_pool={}",
                        vf.0,
                        l_pool,
                        value,
                    )
                    .map_err(|_| QueryError::internal("failed to write query output"))?;
                    writeln!(output, "summary record_count=1 invalid_count=0")
                        .map_err(|_| QueryError::internal("failed to write query output"))?;
                } else {
                    for (l_pool, vl_pool) in map_table.iter().enumerate() {
                        writeln!(
                            output,
                            "record=map-entry vf={} l_pool={} vl_pool={}",
                            vf.0,
                            l_pool,
                            vl_pool,
                        )
                        .map_err(|_| QueryError::internal("failed to write query output"))?;
                    }
                    writeln!(output, "summary record_count=4 invalid_count=0")
                        .map_err(|_| QueryError::internal("failed to write query output"))?;
                }
                Ok(())
            })
            .unwrap();

        Self {
            logger: create_module_logger("bmu", LogLevel::Info).unwrap(),
            state,
            _map_table_query: map_table_query,
        }
    }

    #[named]
    fn send_cmd(&self, opcode: u32) {
        ldebug!(self.logger, "send command opcode={:#x}", opcode);
        linfo!(self.logger, "command submitted");
        lhexdump!(self.logger, LogLevel::Debug, [0x01_u8, 0x23, 0x45, 0x67]);

        if opcode == 0xffff {
            lerror!(self.logger, "invalid opcode={:#x}", opcode);
        }
    }
}

fn main() {
    let bmu = Bmu::new("0");
    bmu.send_cmd(0x17);
}
