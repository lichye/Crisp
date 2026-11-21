// A real error: the parent passes its input through unchanged.
module bug_child(input clk, input [3:0] x);
    p_small: assert property (@(posedge clk) x != 4'hf);
endmodule

module bug_top(input clk, input [3:0] a);
    bug_child u_child(.clk(clk), .x(a));
endmodule
