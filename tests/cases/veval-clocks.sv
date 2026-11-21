// Two assertions sampled on two different clocks.
module two_clocks(input clk_a, input clk_b, input [3:0] a);
    p_a: assert property (@(posedge clk_a) a == a);
    p_b: assert property (@(posedge clk_b) a == a);
endmodule
