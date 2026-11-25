// Spec 23 section 8: one compile-time out-of-range read. t[3:1] selects bit 3
// of a 3-bit vector; IEEE 1800-2017 11.5.1 reads it as x.
module oob(input clk, input [2:0] t);
    wire [2:0] s = t[3:1];
    p_high_zero: assert property (@(posedge clk) s[2] == 1'b0);
endmodule
