// rtl-oob.sv with the out-of-range bit written as an explicit x.
module oob_x(input clk, input [2:0] t);
    wire [2:0] s = {1'bx, t[2:1]};
    p_high_zero: assert property (@(posedge clk) s[2] == 1'b0);
endmodule
