module undriven_temporal(input bit clk);
  wire en;
  reg previous;
  bit valid = 0;
  always @(posedge clk) begin
    previous <= en;
    valid <= 1;
  end
  p_stable: assert property (@(posedge clk) !valid || en == previous);
endmodule
