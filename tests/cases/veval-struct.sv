// The child's port is a struct and its assertion reads a field: the product
// must keep the port's type, or the field read does not elaborate.
package veval_struct_pkg;
    typedef struct packed {
        logic [3:0] op;
        logic valid;
    } command_t;
endpackage

module struct_child(input clk, input veval_struct_pkg::command_t command);
    p_field: assert property (@(posedge clk) command.valid |-> (command.op & 4'h7) != 4'hf);
endmodule

module struct_top(input clk, input [2:0] a, input v);
    veval_struct_pkg::command_t command;
    always @(posedge clk) command <= '{op: {1'b0, a}, valid: v};
    struct_child u_child(.clk(clk), .command(command));
endmodule
