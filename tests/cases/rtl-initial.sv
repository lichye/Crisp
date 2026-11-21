// IEEE 1800-2017 9.2.2.4: a variable written by always_ff may not be written by initial.
module initial_and_always_ff(input clk, input addr, input [3:0] data);
    logic [3:0] mem [0:1];
    initial begin
        mem[0] = 0;
        mem[1] = 0;
    end
    always_ff @(posedge clk) mem[addr] <= data;
    p_written: assert property (@(posedge clk) 1 |=> mem[$past(addr)] == $past(data));
endmodule
