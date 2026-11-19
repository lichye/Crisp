module dynamic_word_bit(input logic clk, input logic [2:0] a, input logic b);
  logic [3:0] mem [0:3];
  always_ff @(posedge clk) mem[a][1] <= b;
endmodule

module dynamic_row_column(input logic clk, input logic [2:0] a, c, input logic [3:0] d);
  logic [3:0] mem [0:3][0:3];
  always_ff @(posedge clk) mem[a][c] <= d;
endmodule

module fixed_column(input logic clk, input logic [2:0] a, input logic [3:0] d);
  logic [3:0] mem [0:3][0:3];
  always_ff @(posedge clk) mem[a][1] <= d;
endmodule

module combinational_bit(input logic [2:0] a, input logic b, output logic [3:0] mem [0:3]);
  always_comb begin
    for (int i = 0; i < 4; i++) mem[i] = 0;
    mem[a][2] = b;
  end
endmodule

module out_of_bounds_bit(input logic clk, input logic b);
  logic [3:0] mem [0:3];
  always_ff @(posedge clk) mem[5][1] <= b;
endmodule
