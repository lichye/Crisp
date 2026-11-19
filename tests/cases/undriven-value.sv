module undriven_value;
  wire en;
  wire [1:0] y = en ? 2'b01 : 2'b10;
  p_nonzero: assert property (y != 0);
endmodule
