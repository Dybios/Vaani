## Vaani: Voice Clarity Application

Vaani is a simple noise suppression application which installs on your selected microphone to reduce background noise. This application works by directly installing the suppression algorithms as a Windows' APO effect. So it works through some simple registry manipulation without any additional installations. This effect works if Device Default Effects is enabled for the selected microphone (enabled by default) under Windows' Sound Settings.

The algorithm uses RNNoise recompiled as a Windows' static library from [here](https://github.com/Dybios/rnnoise-windows), which is then compiled as an APO DLL from our [RNNoiseAPO](https://github.com/Dybios/RNNoiseAPO) sister repository. The APO DLL creation is an extension of the MinimalAPO example provided in EqualizerAPO's [developer documentation](https://sourceforge.net/p/equalizerapo/wiki/Developer%20documentation/).

To install, go to [Releases](https://github.com/Dybios/Vaani/releases) page and download the executable `Vaani.exe` from the latest released version. Follow the steps and reboot the system to start applying this effect on your microphone.

**NOTE:** _Vaani will only be active on the microphone you have installed it on. Other microphones will not be affected unless you choose to install on them explicitly._
