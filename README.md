# YouTube Quality

[Download YouTube Quality for Windows x64](https://github.com/mugpet/youtube-quality/releases/latest/download/YouTube-Quality.exe). Save the executable where you want to keep it and double-click it. Its play icon appears in the Windows notification area (sometimes under the up arrow). Use YouTube in your usual Google Chrome or Microsoft Edge windows, with the account already signed in to each browser.

## Playback

On a video with a **Settings → Quality** menu, the app aims for the highest offered resolution up to 1440p. It prefers enhanced bitrate when it can identify a playable Premium option. If the best setting is already selected, or **Auto** is already delivering it, the app leaves playback alone. After a reload, it gives Auto time to settle before correcting a quality mismatch. You might briefly see the player menu open and close while the app reads the quality. YouTube controls quality on layouts that omit the Quality menu, including some Shorts.

The helper follows the selected tab of whichever Chrome or Edge window is in front. It checks when you switch videos or windows, then about every two minutes during playback. Hidden tabs are checked when you bring them forward. It uses the English or Danish player menu and leaves a menu you already opened alone.

The helper stays in the tray even when you close both browsers. Right-click its icon to choose **Exit YouTube Quality**, or enable **Start with Windows** if you want it to run after your next Windows sign-in. Opening the desktop app again while it is running shows the same tray menu.

## Build

The standalone Windows app lives in `desktop`. Build it with `desktop/Build.ps1` using Visual Studio 2022 with the C++ desktop workload. It uses the browsers installed on your PC and does not store account credentials. To check the playback-quality decisions, compile and run `test/quality-format.test.cpp` with a C++17 compiler.

The separate browser-extension edition is in `extension`. Its tests run with `node --test test/quality.test.cjs`. The optional `Setup.cmd` sets up that edition in Chrome.
