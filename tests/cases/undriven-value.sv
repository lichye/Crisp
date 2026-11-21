module undriven_value;
  wire en;
  wire [3:0] bus;
  assign bus[2:0] = 3'b000;
  wire [1:0] y = (en ^ bus[3]) ? 2'b01 : 2'b10;
  always_comb p_nonzero: assert (y != 0);
endmodule
