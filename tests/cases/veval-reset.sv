// The child's register starts from its reset. The landing gets the reset from
// the top's reset contract or, with NO_CONTRACT, from [design] reset rst high.
module reset_child(input clk, input rst, input [3:0] a);
    reg [3:0] r;
    always @(posedge clk) if (rst) r <= 0; else if (a < 4'd8) r <= a;
    p_small: assert property (@(posedge clk) disable iff (rst) r < 4'd8);
endmodule

module reset_top(input clk, input rst, input [3:0] a);
`ifndef NO_CONTRACT
    reset_contract_rst: assume property (@(posedge clk) rst);
`endif
    reset_child u_child(.clk(clk), .rst(rst), .a(a));
endmodule

module alias_middle(input sample_clk, sample_rst, input [3:0] a);
    reset_child u_leaf(.clk(sample_clk), .rst(sample_rst), .a(a));
endmodule

module alias_top(input board_clk, board_rst, input [3:0] a);
    alias_middle u_mid(.sample_clk(board_clk), .sample_rst(board_rst), .a(a));
    p_parent: assert property (@(posedge board_clk) disable iff (board_rst) u_mid.u_leaf.r < 4'd8);
endmodule
