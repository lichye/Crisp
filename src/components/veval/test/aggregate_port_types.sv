`include "aggregate_port_types.svh"

module aggregate_dut(
    input logic clk,
    input logic [1:0][3:0] words[4:2],
    input logic [0:0] flags[2],
    input aggregate_pkg::state_t state,
    input aggregate_pkg::enable_t enable,
    output aggregate_result_t result
);
    assign result.data = words[3][1];
    assign result.valid = state == aggregate_pkg::ACTIVE && enable == aggregate_pkg::ON && flags[1][0];
endmodule

module aggregate_boundary(
    input logic clk,
    input logic [1:0][3:0] words[4:2],
    input logic [0:0] flags[2],
    input aggregate_pkg::state_t state,
    input aggregate_pkg::enable_t enable
);
    aggregate_result_t result;
    aggregate_dut dut(.*);
    reads_elements_and_fields: assert property (@(posedge clk)
        result.valid |-> result.data == words[3][1]);
    reads_enum: assert property (@(posedge clk)
        result.valid == (state == aggregate_pkg::ACTIVE && enable == aggregate_pkg::ON && flags[1][0]));
endmodule
