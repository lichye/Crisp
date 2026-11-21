// IEEE 1800-2017 6.21: q is used before its declaration.
module use_before_declaration(input clk, input [3:0] a);
    always_ff @(posedge clk) q <= a;
    logic [3:0] q;
    p_follows: assert property (@(posedge clk) 1 |=> q == $past(a));
endmodule
