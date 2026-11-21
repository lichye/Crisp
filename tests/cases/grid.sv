// A two-dimensional array written at one element and read at a dynamic
// index. EBMC's SMT encoding contains a quantifier, and Yices 2.7 rejects
// it: "quantifiers are not allowed in logic QF_AUFBV".
module grid(input clk, input row, col, input [3:0] x);
    reg [3:0] slot [0:1][0:1];
    always @(posedge clk) slot[0][1] <= x;
    p_kept: assert property (@(posedge clk) 1 |=> slot[0][1] == $past(x));
    p_zero: assert property (@(posedge clk) slot[row][col] == 0);
endmodule
