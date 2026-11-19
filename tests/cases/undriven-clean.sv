module undriven_clean(input bit source);
  wire ignored;
  wire driven = source;
  p_driven: assert property (driven == source);
endmodule
