// The child's assertion holds only under the parent's assumption, which veval lowers to it.
module assumed_child(input clk, input [3:0] x);
    p_child: assert property (@(posedge clk) x != 4'hf);
endmodule

module assumed_top(input clk, input [3:0] a);
`ifndef NO_ASSUME
    a_not_15: assume property (@(posedge clk) a != 4'hf);
`endif
    assumed_child u_child(.clk(clk), .x(a));
endmodule
