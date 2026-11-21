// Two child assertions; properties selects one of them for veval.
module two_child(input clk, input [3:0] x);
    p_one: assert property (@(posedge clk) x != 4'hf);
    p_two: assert property (@(posedge clk) x != 4'he);
endmodule

module two_top(input clk, input [3:0] a);
    a_small: assume property (@(posedge clk) a < 4'd8);
    two_child u_child(.clk(clk), .x(a));
endmodule
