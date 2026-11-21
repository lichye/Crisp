module ctl_witness(input bit clk, finish);
  bit [1:0] phase = 0;
  always @(posedge clk) phase <= phase + 1;

  eventually_finishes: assert property (@(posedge clk) s_eventually finish);
  finishes_in_phase: assert property (@(posedge clk) (!finish) s_until (finish && phase == 1));
endmodule
