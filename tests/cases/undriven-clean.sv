module undriven_clean(input bit source);
  wire ignored;
  wire driven = source;
  always_comb p_driven: assert (driven == source);
endmodule
