%VALIDATE_C_OUTPUT Compare the C firmware filters with the MATLAB reference.
%   Run from the matlab/ folder (requires build/sensor_node.exe):
%     matlab -batch "cd matlab; validate_c_output"
%   Writes ../experiments/results/matlab_validation.txt and
%          ../experiments/results/plots/matlab_filter_validation.png
here = fileparts(mfilename('fullpath'));
root = fileparts(here);
exe  = fullfile(root, 'build', 'sensor_node.exe');
res  = fullfile(root, 'experiments', 'results');
if ~exist(fullfile(res, 'plots'), 'dir'), mkdir(fullfile(res, 'plots')); end

% deterministic test signal: slow wave + noise + a step + isolated spikes
rng(1);
n = 600; t = (0:n-1)';
x = 7 + 0.3*sin(2*pi*t/150) + 0.05*randn(n,1);
x(300:end) = x(300:end) - 1.0;                 % step (genuine change)
x([80 210 420 500]) = x([80 210 420 500]) + [2 -2 2.5 -1.5]';   % spikes
in_csv  = fullfile(res, 'filter_input.csv');
out_csv = fullfile(res, 'filter_c_output.csv');
writematrix(x, in_csv);

[st, msg] = system(sprintf('"%s" --filter-test "%s" "%s"', exe, in_csv, out_csv));
assert(st == 0, msg);
c = readmatrix(out_csv);                       % columns: x, ma5, fir7, hampel
[ma5, fir7, hmp] = reference_filter(x);

err = [max(abs(c(:,2) - ma5)), max(abs(c(:,3) - fir7)), max(abs(c(:,4) - hmp))];
names = {'moving average (5)', 'FIR binomial (7)', 'Hampel (5, k=3)'};
tol = 1e-4;                                    % C uses single precision
fid = fopen(fullfile(res, 'matlab_validation.txt'), 'w');
fprintf(fid, 'MATLAB %s reference vs C firmware filters (%d samples)\n', version, n);
for k = 1:3
    fprintf(fid, '%-20s max |C - MATLAB| = %.3g  %s\n', names{k}, err(k), ternary(err(k) < tol));
end
fclose(fid);
type(fullfile(res, 'matlab_validation.txt'));

f = figure('Visible', 'off', 'Position', [100 100 1000 600]);
subplot(2,1,1);
plot(t, x, 'Color', [0.6 0.6 0.6]); hold on;
plot(t, c(:,4), 'Color', [0.92 0.41 0.20], 'LineWidth', 1.2);
plot(t, c(:,3), 'Color', [0.16 0.47 0.84], 'LineWidth', 1.5);
plot(t, fir7, 'k:', 'LineWidth', 1.2);
legend('input', 'C Hampel', 'C FIR (after raw input)', 'MATLAB FIR', 'Location', 'southwest');
title('Firmware C filters vs MATLAB reference'); ylabel('value');
subplot(2,1,2);
semilogy(t, abs(c(:,2) - ma5) + 1e-12, t, abs(c(:,3) - fir7) + 1e-12, t, abs(c(:,4) - hmp) + 1e-12);
legend(names, 'Location', 'northeast'); ylabel('|C - MATLAB|'); xlabel('sample');
title('Absolute difference (single-precision rounding level)');
exportgraphics(f, fullfile(res, 'plots', 'matlab_filter_validation.png'), 'Resolution', 130);
assert(all(err < tol), 'C output deviates from MATLAB reference');

function s = ternary(ok)
if ok, s = 'PASS'; else, s = 'FAIL'; end
end
