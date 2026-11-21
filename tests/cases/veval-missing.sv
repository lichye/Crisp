// The assumption is over a + b; only a reaches the child, so it cannot be lowered there.
module missing_child(input clk, input [3:0] x);
    p_child: assert property (@(posedge clk) x != 4'hf);
endmodule

module missing_top(input clk, input [3:0] a, input [3:0] b);
    sum_small: assume property (@(posedge clk) a + b < 4'd8);
    missing_child u_child(.clk(clk), .x(a));
endmodule
