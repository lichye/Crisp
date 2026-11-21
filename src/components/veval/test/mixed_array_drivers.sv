module mixed(input logic clk, d, output logic y);
  // A nonzero, descending range checks that the storage uses internal indices.
  logic lane[8:7];
  assign lane[8] = d;
  initial lane[7] = 1'b0;
  always_ff @(posedge clk) lane[7] <= lane[8] ^ lane[7];
  assign y = lane[7];
endmodule

module mixed_comb(input logic d, output logic y);
  logic lane[2];
  always_comb lane[1] = !lane[0];
  assign lane[0] = d;
  assign y = lane[1];
endmodule

module overlap(input logic clk, d);
  logic lane[2];
  always_ff @(posedge clk) lane[1] <= d;
  assign lane[1] = d;
endmodule

module dynamic_overlap(input logic clk, addr, d);
  logic lane[2];
  assign lane[1] = d;
  always_ff @(posedge clk) lane[addr] <= d;
endmodule

module static_outer_prefix(input logic clk, addr, d);
  logic lane[2][2];
  always @(posedge clk) lane[0] <= '{default:0};
  always @(posedge clk) lane[1][addr] <= d;
endmodule
