function [ma5, fir7, hmp] = reference_filter(x)
%REFERENCE_FILTER Independent MATLAB reference for the firmware DSP chain.
%   [ma5, fir7, hmp] = reference_filter(x) returns
%     ma5  - 5-point moving average (firmware ma_filter_t, len 5)
%     fir7 - 7-tap binomial FIR [1 6 15 20 15 6 1]/64 (FIR_BINOMIAL7)
%     hmp  - causal Hampel identifier, window 5, k = 3, min_dev = 0
%   The firmware primes its delay lines with the first sample, so the
%   reference does the same by pre-padding x(1).
x = x(:);

% moving average via filter() with primed initial history
b_ma = ones(5,1) / 5;
y = filter(b_ma, 1, [repmat(x(1), 4, 1); x]);
ma5 = y(5:end);

% FIR via filter() with primed initial history
b_fir = [1 6 15 20 15 6 1]' / 64;
y = filter(b_fir, 1, [repmat(x(1), 6, 1); x]);
fir7 = y(7:end);

% causal Hampel: window = last up-to-5 raw samples including the current one
hmp = x;
for n = 3:numel(x)
    w = x(max(1, n-4):n);
    med = median(w);
    mad = median(abs(w - med));
    lim = 3 * 1.4826 * mad;
    if abs(x(n) - med) > lim
        hmp(n) = med;
    end
end
end
