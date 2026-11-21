`ifndef VEVAL_AGGREGATE_PORT_TYPES_SVH
`define VEVAL_AGGREGATE_PORT_TYPES_SVH
typedef struct {
    logic [3:0] data;
    logic valid;
} aggregate_result_t;

package aggregate_pkg;
    typedef enum logic [1:0] {IDLE, ACTIVE} state_t;
    typedef enum logic [1:0] {OFF, ON} enable_t;
endpackage
`endif
