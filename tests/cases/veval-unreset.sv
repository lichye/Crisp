// The top's reset does not reach the child, whose check runs without it.
module unreset_child(input clk, input [3:0] x);
    p_any: assert property (@(posedge clk) x == x);
endmodule

module unreset_top(input clk, input rst, input [3:0] a);
    unreset_child u_child(.clk(clk), .x(a));
endmodule
