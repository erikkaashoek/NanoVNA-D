# Control NanoVNA from python script

NanoVNA is able to be controlled via USB serial interface from PC. There are sample scripts in this directory.

## Preparation

    $ cd python
    $ pip3 install -r requirements.txt

## Run

### Plot reflection LOGMAG.

    $ ./nanovna.py -p

### Plot transmission LOGMAG.

    $ ./nanovna.py -p -P 1

### Plot smithchart.

    $ ./nanovna.py -s

### Capture display

    $ ./nanovna.py -C out.png

### Show usage.

    $ ./nanovna.py -h

## Live FFT of the raw sample stream

`pfa_stream.py` puts the device into BULK mode (CONFIG -> MODE -> BULK STREAM),
reads the raw I2S blocks off the CDC and plots the spectrum of both channels.

    $ python pfa_stream.py                 # live plot
    $ python pfa_stream.py --text          # no GUI, statistics and peak bins
    $ python pfa_stream.py --raw           # skip the chain-response correction
    $ python pfa_stream.py -h

It restores NORMAL mode on exit. Notes:

* The link carries the full 192 kHz stereo rate (~780 kB/s) essentially
  losslessly, so contiguous 64 ms windows (15.6 Hz bins) are the norm. Blocks
  are only joined when the sequence number advances *and* the drop counter is
  unchanged, so the displayed resolution always reflects real contiguous data.
* Even samples are the reference input (IN3, codec L), odd the signal input
  (IN1, codec R).
* By default the measured mixer/codec response is divided out (-3 dB at 28 kHz,
  -15 dB at 88 kHz). Corrected levels can exceed 0 dBFS - that is the level
  referred to a flat chain, not clipping. Use `--raw` for the ADC's own view.
* Set the gain before streaming with `gain N` (0-95, 0.5 dB steps): AGC cannot
  run while the DSP is stopped, so it stays frozen at whatever it last chose.

## Using in Jupyter Notebook

To use NanoVNA from Jupyter notebook, see [this page](/python/NanoVNA-example.ipynb).
