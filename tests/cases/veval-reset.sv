// The child's register starts from its reset; the top's reset contract says
// rst is asserted at the initial instant, which the landing needs.
module reset_child(input clk, input rst, input [3:0] a);
    reg [3:0] r;
    always @(posedge clk) if (rst) r <= 0; else if (a < 4'd8) r <= a;
    p_small: assert property (@(posedge clk) disable iff (rst) r < 4'd8);
endmodule

module reset_top(input clk, input rst, input [3:0] a);
    reset_contract_rst: assume property (@(posedge clk) rst);
    reset_child u_child(.clk(clk), .rst(rst), .a(a));
endmodule
