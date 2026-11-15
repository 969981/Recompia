<!-- source-sha256: c634f3c7ee8ae03825b35d79f4ee7e47a222a83971d3505d3bebb09e0b52218d -->

> यह अंग्रेज़ी README (../../README.md) का मशीनी अनुवाद है। इसमें गलतियाँ हो सकती हैं, और अंग्रेज़ी README ही आधिकारिक है।

<h1 align="center">
  <img src="../assets/logo.png" alt="Recompia" width="420">
</h1>

<h3 align="center">Pokopia Recomp</h3>

<p align="center">
  <b>पोकीमोन पोकोपिया (Switch 2) को रीकंपाइल करके Windows पर सीधे चलाने के लिए, मल्टीप्लेयर समर्थन के साथ।</b>
</p>

<p align="center">
  <a href="https://github.com/featureJosh/pokopia-recomp/releases/latest"><img src="https://img.shields.io/github/v/release/featureJosh/pokopia-recomp?label=latest&color=d8b46a&labelColor=1b2740" alt="Latest release"></a>
  <a href="https://github.com/featureJosh/pokopia-recomp/releases/latest"><img src="https://img.shields.io/endpoint?url=https%3A%2F%2Fraw.githubusercontent.com%2FDittobyte%2Fpokopia%2Fbadges%2Flatest.json&label=latest%20downloads&labelColor=1b2740" alt="Latest release downloads"></a>
  <a href="https://github.com/featureJosh/pokopia-recomp/releases"><img src="https://img.shields.io/endpoint?url=https%3A%2F%2Fraw.githubusercontent.com%2FDittobyte%2Fpokopia%2Fbadges%2Fall.json&label=all%20downloads&labelColor=1b2740" alt="All downloads">
  <img src="https://img.shields.io/endpoint?url=https%3A%2F%2Fraw.githubusercontent.com%2FDittobyte%2Fpokopia%2Fbadges%2Fwin64.json&label=Windows%20downloads&labelColor=1b2740" alt="Windows downloads">
  <img src="https://img.shields.io/endpoint?url=https%3A%2F%2Fraw.githubusercontent.com%2FDittobyte%2Fpokopia%2Fbadges%2Flinux-x64.json&label=Linux%20downloads&labelColor=1b2740" alt="Linux downloads"></a>
  <a href="https://github.com/featureJosh/pokopia-recomp/releases"><img src="https://img.shields.io/endpoint?url=https%3A%2F%2Fraw.githubusercontent.com%2FDittobyte%2Fpokopia%2Fbadges%2Ftesting.json&label=testing&labelColor=1b2740" alt="Latest Testing build"></a>
  <img src="https://img.shields.io/badge/platform-Windows%2010%2F11-0078d6?logo=windows&logoColor=white&labelColor=1b2740" alt="Platform: Windows 10/11">
  <img src="https://img.shields.io/badge/platform-Linux%20AppImage-e95420?logo=linux&logoColor=white&labelColor=1b2740" alt="Platform: Linux AppImage">
  <a href="https://github.com/featureJosh/pokopia-recomp/commits/main"><img src="https://img.shields.io/github/last-commit/featureJosh/pokopia-recomp/main?label=last%20commit&color=58a6ff&labelColor=1b2740" alt="Last commit on main"></a>
  <a href="https://github.com/featureJosh/pokopia-recomp/commits/testing"><img src="https://img.shields.io/github/last-commit/featureJosh/pokopia-recomp/testing?label=testing%20commit&color=d29922&labelColor=1b2740" alt="Last commit on testing"></a>
  <a href="https://github.com/featureJosh/pokopia-recomp/issues"><img src="https://img.shields.io/github/issues/featureJosh/pokopia-recomp?label=open%20issues&color=db61a2&labelColor=1b2740" alt="Open issues"></a>
</p>

<p align="center">
  <a href="https://github.com/featureJosh/pokopia-recomp/releases/latest"><img src="https://img.shields.io/badge/%E2%AC%87_Download-latest_for_Windows-d8b46a?style=for-the-badge&labelColor=1b2740" alt="Download the latest release for Windows"></a>
  <a href="https://github.com/featureJosh/pokopia-recomp/releases/latest"><img src="https://img.shields.io/badge/%E2%AC%87_Download-latest_for_Linux_(AppImage)-e95420?style=for-the-badge&labelColor=1b2740" alt="Download the latest release for Linux (AppImage)"></a>
</p>

<p align="center">
  <b><a href="https://github.com/featureJosh/pokopia-recomp/releases">रिलीज़</a></b> ·
  <a href="../../CHANGELOG.md">चेंजलॉग</a> ·
  <a href="../../CONTRIBUTING.md">योगदान</a> ·
  <a href="../../CREDITS.md">श्रेय</a> ·
  <a href="https://github.com/xFractals/pokopia-native">मूल: pokopia-native</a> ·
  <a href="../../SECURITY.md">सुरक्षा</a> ·
  <a href="../../LEGAL.md">कानूनी</a> ·
  <a href="https://github.com/featureJosh/pokopia-recomp/issues">समस्याएँ</a> ·
  <a href="https://discord.recompia.com">Discord</a>
</p>

> [!IMPORTANT]
> **इसमें गेम की कोई फ़ाइल शामिल नहीं है।** आपको गेम की अपनी एक्सट्रैक्ट की गई कॉपी चाहिए, वर्ज़न **2.0.1a**। यह प्रोजेक्ट कोई गेम फ़ाइल, key या डंपिंग टूल नहीं देता। Nintendo, The Pokémon Company या Koei Tecmo से इसका कोई संबंध नहीं है; Pokémon, Nintendo, Creatures Inc. और GAME FREAK inc. का ट्रेडमार्क है (देखें [LEGAL.md](../../LEGAL.md))।

> [!NOTE]
> **AI की मदद से बनाया गया, इंसान की निगरानी में।** यह प्रोजेक्ट मुख्य रूप से AI के साथ बनाया गया है, और मैं इसका सुपरवाइज़र हूँ। मेरे पास कुछ सालों का डेवलपमेंट अनुभव है, इसलिए यह सब बेकार नहीं है। हर commit को मैं खुद जाँचता, सत्यापित करता और टेस्ट करता हूँ, तभी वह शामिल होता है।

## स्क्रीनशॉट

<table>
  <tr>
    <td width="50%"><img src="../assets/screenshots/title-screen.svg" alt="Title screen"><br><sub>शीर्षक स्क्रीन</sub></td>
    <td width="50%"><img src="../assets/screenshots/island-gameplay.svg" alt="Island gameplay"><br><sub>अपना द्वीप बनाना</sub></td>
  </tr>
  <tr>
    <td width="50%"><img src="../assets/screenshots/aspect-ratios.svg" alt="New Aspect Ratios"><br><sub>नए आस्पेक्ट रेशियो पर खेलें</sub></td>
    <td width="50%"><img src="../assets/screenshots/link-play.svg" alt="Link Play (screenshot placeholder)"><br><sub>दोस्त के साथ Link Play</sub></td>
  </tr>
</table>

## विशेषताएँ

### साथ में खेलें (Link Play)
- **किसी दोस्त के द्वीप पर जाएँ, या उसे अपने द्वीप पर बुलाएँ**, एक ही Wi-Fi पर या इंटरनेट के ज़रिए।
- **गेम वर्ज़न 2.0.1 पर [pokopia-native](https://github.com/xFractals/pokopia-native) खिलाड़ियों के साथ क्रॉस-प्ले।**
- इसे लॉन्चर के **Link Play** पेज में सेट करें। कोई फ़ाइल एडिट नहीं करनी।
- चरण-दर-चरण गाइड: [साथ में खेलना](#playing-together)।

### अपने सेव इंपोर्ट करें
- **पहले से Eden या Ryujinx पर खेल रहे हैं?** **Settings** में एक बटन आपका सेव ढूँढकर ले आता है। आपके एमुलेटर की कॉपी को छुआ नहीं जाता।
- **सेव कहीं और है?** उसका फ़ोल्डर चुनें (वह जिसमें `B00`, `G00` और `S00` अंदर हैं)।
- **आपका मौजूदा सेव कभी नहीं मिटाया जाता।** पहले उसे बैकअप फ़ोल्डर के रूप में रखा जाता है।
- अगर सेव लिखा नहीं जा सकता (डिस्क भरी है, या OneDrive या एंटीवायरस फ़ाइल रोके हुए है), तो आपका आख़िरी सही सेव बचा रहता है और गेम आपको बता देता है।

### आसान सेटअप
- **अपना डंप जैसा है वैसे ही इस्तेमाल करें:** एक फ़ोल्डर (`base` और `updates` के साथ) या एक `.nup` या `.nx` फ़ाइल।
- **डंप में कोई दिक्कत हो तो साफ़ संदेश:** गेम का गलत वर्ज़न, छूटा हुआ अपडेट, या कॉपी करते समय खराब हुई फ़ाइलें। लॉन्चर बताता है क्या करना है।
- **शुरू होने से पहले जाँच:** पर्याप्त डिस्क स्पेस, काम करता ग्राफ़िक्स ड्राइवर, पर्याप्त मेमोरी।
- **Use fewer cores** बिल्ड के दौरान आपका PC इस्तेमाल के लायक रहता है। आप रोककर बाद में जारी रख सकते हैं।
- **अपडेट तेज़ हैं:** सिर्फ़ बदले हुए हिस्से फिर से बनाए जाते हैं।
- लॉन्चर बताता है जब नया वर्ज़न आता है।

### कंट्रोल
- **कोई भी कंट्रोलर:** Xbox, PlayStation, Switch Pro और अन्य, रंबल और मोशन के साथ। बटन डिफ़ॉल्ट रूप से Switch लेआउट के अनुसार।
- **कीबोर्ड और माउस।** माउस मेन्यू में भी चलता है: चुने हुए आइटम की पुष्टि के लिए क्लिक करें; चयन को खिसकाने के लिए स्क्रॉल करें या ऐरो कुंजियाँ इस्तेमाल करें।
- **कोई भी बटन बदलें**, F1 के **Controls** पेज पर।
- **Joy-Con 2 माउस मोड** आइटम रखने के लिए: माउस का बीच का बटन दबाएँ।
- **स्क्रीन पर बटन आइकन वही दिखते हैं जो आपके हाथ में है:** कीबोर्ड, Xbox, PlayStation या Switch।
- **अपना नाम टाइप करना** कंट्रोलर, माउस या कीबोर्ड से चलता है।
- **Alt+F4 के बिना बाहर निकलें:** Select और Start दबाए रखें (Xbox पैड पर View और Menu), या F1 में **Quit game** इस्तेमाल करें।
- **कंट्रोलर से F1 मेन्यू खोलें:** दोनों स्टिक एक साथ दबाएँ (L3 + R3); `controls.ini` में `[overlay] pad` से बदलें या बंद करें (`none` = बंद)। यह क्विट कॉम्बिनेशन नहीं हो सकता।

### Steam Deck और हैंडहेल्ड
- Steam Deck, Legion Go S और दूसरे हैंडहेल्ड पहचाने जाते हैं: कंट्रोलर आइकन स्क्रीन पर रहते हैं और तस्वीर का आकार स्क्रीन के मुताबिक़ होता है।
- 16:10 स्क्रीन किनारे तक भरी जाती हैं, काली पट्टियों के बिना।
- SteamOS पर Proton के ज़रिए चलता है, हैंडहेल्ड स्पीड-अप के साथ (Steam Deck पर लगभग 26 से 53 fps और Legion Go S पर 55 से 75 fps, खिलाड़ियों के मापन के अनुसार)।
- लॉन्चर Linux और SteamOS पर भी चलता है: देखें [Linux, SteamOS और Steam Deck](#linux-steamos-and-the-steam-deck)।

### ग्राफ़िक्स
- **किसी भी रेज़ोल्यूशन पर खेलें**, 4K और उससे ऊपर। धीमा चले तो **Render scale** घटाएँ।
- **क्वालिटी प्रीसेट** (Switch 2, High, Ultra) तय करते हैं कि आप कितनी दूर देख सकते हैं और टेक्सचर कितने तेज़ हैं।
- **HDR** HDR स्क्रीन पर।
- **विंडो, बॉर्डरलेस या फ़ुल स्क्रीन।** मॉनिटर चुनें, फ़्रेम रेट सीमित करें, FPS काउंटर दिखाएँ। खेलते समय Alt+Enter मोड बदलता है।
- **अपस्केलर:** FSR 1 और NVIDIA Image Scaling काम करते हैं।
- **FSR 3 और DLSS / DLAA मौजूद हैं पर अभी अच्छे नहीं दिखते** और इन्हें फिर से बनाना होगा। अभी इन्हें बंद ही रखें।

### गेम के अंदर सेटिंग्स
गेम में **F1** दबाकर ग्राफ़िक्स, कंट्रोल और आवाज़ (वॉल्यूम, आउटपुट डिवाइस, विंडो पीछे होने पर म्यूट) बदलें। ज़्यादातर बदलाव तुरंत लागू होते हैं।

## त्वरित शुरुआत

1. [नवीनतम रिलीज़](https://github.com/featureJosh/pokopia-recomp/releases/latest) से zip डाउनलोड करें और कहीं भी एक्सट्रैक्ट करें।
2. `PokopiaLauncher.exe` चलाएँ। अगर SmartScreen चेतावनी दे, तो **More info → Run anyway** चुनें। अपना डंप फ़ोल्डर चुनें और **Set up** दबाएँ: यह आपके PC पर गेम कंपाइल करता है (लगभग 7 मिनट), फिर शेडर कैश बनाता है (1 से 2 घंटे, आप रोककर बाद में जारी रख सकते हैं)।
3. **Play** दबाएँ।

F1 गेम में सेटिंग्स खोलता है। माउस का बीच का बटन आइटम रखने के लिए Joy-Con 2 माउस मोड बदलता है।

### Linux, SteamOS और Steam Deck

1. Steam में **Proton Experimental** इंस्टॉल करें (Library, Tools; Steam Deck पर पहले Desktop Mode में जाएँ)।
2. [नवीनतम रिलीज़](https://github.com/featureJosh/pokopia-recomp/releases/latest) से `PokopiaRecomp-<version>-linux-x64.tar.gz` डाउनलोड करें और कहीं भी एक्सट्रैक्ट करें (जैसे अपने home फ़ोल्डर में)।
3. उस फ़ोल्डर में `PokopiaLauncher.AppImage` चलाएँ (उस पर डबल-क्लिक करें, या टर्मिनल में `./PokopiaLauncher.AppImage`)। अपना डंप फ़ोल्डर चुनें और **Set up** दबाएँ: पहली बार यह अपने बिल्ड टूल डाउनलोड करता है (Python और Zig, लगभग 100 MB), गेम कंपाइल करता है, फिर Proton के ज़रिए शेडर कैश बनाता है। आप रोककर बाद में जारी रख सकते हैं।
4. **Play** दबाएँ। गेम खुद Steam के Proton के ज़रिए चलता है।

अगर FUSE के बिना सिस्टम पर `PokopiaLauncher.AppImage` शुरू नहीं होता, तो `./PokopiaLauncher.AppImage --appimage-extract-and-run` चलाएँ।

Steam से गेम शुरू करने के लिए (Deck पर Game Mode): Desktop Mode में **Add a Non-Steam Game**, `PokopiaLauncher.AppImage` चुनें, और उसके लॉन्च विकल्प `--play` सेट करें। ज़्यादा जानकारी और क्या टेस्ट हुआ, [launcher/README.md](../../launcher/README.md#linux-steamos-the-steam-deck-bazzite-other-distributions) में।

## मॉड और अनुवाद

आप गेम डंप को छुए बिना बदली हुई फ़ाइलों का फ़ोल्डर (टेक्स्ट या टेक्सचर मॉड, अनुवाद) गेम पर लगा सकते हैं: **Settings > Game > Mods**, या `pkp.ini` में `[fs]` के नीचे `romfs_mod`। इस प्रोजेक्ट के साथ कुछ भी शामिल नहीं है; मॉड उसके लेखक से लें।

आपका चुना फ़ोल्डर गेम के अपने `romfs` जैसा दिखना चाहिए: वही फ़ाइलें और फ़ोल्डर, सबसे ऊपरी स्तर से शुरू। इसकी फ़ाइलें उसी पाथ वाली गेम की फ़ाइलों की जगह लेती हैं, फ़ोल्डर मिलाए जाते हैं, और सिर्फ़ मॉड में मौजूद फ़ाइलें भी दी जाती हैं।

```text
MyMod/                       <- choose this folder in Settings > Game > Mods
├── LD_C0_R.BIN              <- same path as in the game's romfs
├── LI_C0_R.IDX
└── motor_asset_re/
    └── ...
```

अगर डाउनलोड में एक बाहरी फ़ोल्डर है (जैसे `2.0.1/romfs/...`), तो अंदर वाला `romfs` फ़ोल्डर चुनें, जिसमें `LD_C0_R.BIN` है, बाहरी नहीं। अगर फ़ोल्डर का स्तर गलत है, तो `pkp.log` कहता है कि मॉड की कोई भी फ़ाइल गेम की किसी फ़ाइल की जगह नहीं लेती। जिन फ़ाइलों में शेडर हैं, उन्हें बदलने वाले मॉड समर्थित नहीं हैं।

### मॉड और अनुवाद (ब्राज़ीली पुर्तगाली)

आप गेम डंप को छुए बिना बदली हुई फ़ाइलों का फ़ोल्डर (टेक्स्ट या टेक्सचर मॉड, अनुवाद) गेम पर लगा सकते हैं: **Settings > Game > Mods**, या `pkp.ini` में `[fs]` के नीचे `romfs_mod`। इस प्रोजेक्ट में कोई मॉड शामिल नहीं है; मॉड के लेखक से उसे माँगें।

चुने गए फ़ोल्डर की बनावट गेम के `romfs` फ़ोल्डर जैसी होनी चाहिए: वही फ़ाइलें और फ़ोल्डर, सबसे ऊपरी स्तर से। इसकी फ़ाइलें उसी पाथ वाली गेम की फ़ाइलों की जगह लेती हैं, फ़ोल्डर मिलाए जाते हैं, और सिर्फ़ मॉड में मौजूद फ़ाइलें भी इस्तेमाल होती हैं।

अगर डाउनलोड में बाहरी फ़ोल्डर है (जैसे `2.0.1/romfs/...`), तो अंदर वाला `romfs` फ़ोल्डर चुनें, जिसमें `LD_C0_R.BIN` है, बाहरी नहीं। अगर फ़ोल्डर का स्तर गलत है, तो `pkp.log` बताता है कि मॉड की कोई फ़ाइल गेम की किसी फ़ाइल की जगह नहीं लेती। जिन फ़ाइलों में शेडर हैं, उन्हें बदलने वाले मॉड समर्थित नहीं हैं।

## पुराने वर्ज़न से अपडेट करना

आपके सेव, सेटिंग्स और शेडर बने रहते हैं।

**0.7.5 से:** Home पर **Update** दबाएँ (या Settings > About में)। लॉन्चर नई रिलीज़ डाउनलोड करता है, जाँचता है, अपनी फ़ाइलें बदलता है और फिर से शुरू होता है।

**0.7.0 या पुराने से:** [नवीनतम रिलीज़](https://github.com/featureJosh/pokopia-recomp/releases/latest) से नया zip (Windows) या `PokopiaRecomp-<version>-linux-x64.tar.gz` (Linux) डाउनलोड करें, गेम और लॉन्चर बंद करें, और इसे अपने इंस्टॉल पर, उसी फ़ोल्डर में एक्सट्रैक्ट करें। इसमें कोई सेव, सेटिंग, शेडर कैश या गेम फ़ाइल नहीं होती, इसलिए आपकी कोई चीज़ नहीं बदलती।

**अगर अपडेट के बाद Home पर Finish update दिखे:** उस रिलीज़ ने गेम का कोड बदला है, इसलिए लॉन्चर आपकी गेम फ़ाइलों से गेम प्रोग्राम (`pkp.exe`) फिर से बनाता है। ऐसा तब भी होता है जब आपने पहले ऐसी रिलीज़ छोड़ी हो और बाद में अपडेट करें। यह छोटा इंतज़ार है, ज़्यादातर PCs पर लगभग 7 मिनट और हैंडहेल्ड पर थोड़ा ज़्यादा, क्योंकि आपके शेडर और सेव बचे रहते हैं। यह लंबा पहला सेटअप नहीं है: शेडर सिर्फ़ तब फिर अनुवादित होते हैं जब रिलीज़ नोट्स कहें, और तब भी सिर्फ़ बदले हुए। पुराना `pkp.exe` तब तक रहता है जब तक नया तैयार न हो जाए।

बैकअप चाहिए तो पहले `save` फ़ोल्डर कहीं और कॉपी करें। नए वर्ज़न का लिखा सेव पुराने में शायद न खुले।

## साथ में खेलना

Link Play कई खिलाड़ियों को एक द्वीप पर रखता है। क्रॉस-प्ले गेम वर्ज़न **2.0.1** पर pokopia-native खिलाड़ियों के साथ काम करता है। गेम 1.0.5 पर खिलाड़ी शामिल नहीं हो सकते।

**क्या बदला।** Link Play अब pokopia-native का co-op प्रोटोकॉल बोलता है। यह इस पोर्ट के पुराने Link Play बिल्ड्स के साथ काम नहीं करता, इसलिए सब लोग इस रिलीज़ पर अपडेट करें। (लॉन्चर पुराना प्रोटोकॉल **Advanced** के नीचे रखता है, उस सत्र के लिए जहाँ सबके पास अभी भी पुराना बिल्ड हो।)

**हर खिलाड़ी को अपना सेव चाहिए।** किसी और के सेव की कॉपी उसके मूल से जुड़ नहीं सकती।

**सेट करें।** लॉन्चर में **Link Play** खोलें और एक चुनें:

| विकल्प | इसके लिए |
|---|---|
| अकेले | कोई Link Play नहीं। |
| स्वचालित (LAN) | डिफ़ॉल्ट। एक ही Wi-Fi या नेटवर्क: होस्ट अपने आप मिल जाता है। |
| पते से मिलें | एक होस्ट से उसके पते से जुड़ें, जैसे `192.0.2.10`। होस्ट का Link Play पेज इसे दिखाता है। |
| इंटरनेट | रूम कोड के साथ रिले के ज़रिए खेलें। |

बदलाव गेम के अगली बार शुरू होने पर लागू होते हैं।

**गेम में**, अपने पहले घर के PC को खोलें, फिर **Link Play** (यह Withered Wasteland में Environment Level 2 के बाद खुलता है) और **Local connection** चुनें। (**Online connection** Nintendo की अपनी सेवा है और यहाँ काम नहीं करता।) होस्ट **Invite Others to Visit** चुनता है, बाकी **Visit a Friend**।

**एक ही नेटवर्क।** सब **Automatic (LAN)** चुनते हैं। Steam Deck और दूसरे Proton या Linux डिवाइस होस्ट ढूँढते हैं और खुद भी मिल जाते हैं, किसी भी नेटवर्क पर। अगर होस्ट न दिखे, तो आगंतुक **Visit by address** चुनकर होस्ट का LAN पता टाइप करता है। कैफ़े, होटल और गेस्ट Wi-Fi अक्सर डिवाइसों को अलग रखते हैं: वहाँ इंटरनेट के ज़रिए खेलें।

**इंटरनेट के ज़रिए।** सब **Internet** चुनते हैं और रिले के ज़रिए जाते हैं:
- होस्ट **I host** चुनता है। पेज रूम कोड दिखाता है। आगंतुकों को रिले पता और कोड दें। **Copy** और **New code** पेज पर हैं।
- आगंतुक **I visit** चुनता है और रिले पता और कोड टाइप करता है।
- खुद रिले चलाने के लिए, होस्ट के Link Play पेज पर **Run a relay here** दबाएँ (जब तक लॉन्चर खुला है काम करता है), या सर्वर पर `PokopiaLauncher --host` चलाएँ। कोई भी pokopia-native co-op रिले भी काम करता है।
- रिले के बिना, होस्ट पर रिले पता खाली छोड़ें और राउटर पर TCP और UDP **47600** फ़ॉरवर्ड करें। आगंतुक होस्ट के सार्वजनिक पते के साथ **Visit by address** इस्तेमाल करते हैं।
- रिले रूम कोड की जाँच करता है पर गेम का ट्रैफ़िक एन्क्रिप्ट नहीं करता।

**पोर्ट और फ़ायरवॉल।** पहली बार होस्ट करते समय Windows Firewall `pkp.exe` के बारे में पूछता है: **Private networks** की अनुमति दें। आपका नेटवर्क भी Private होना चाहिए (Settings, Network & internet, आपका कनेक्शन, Network profile type: **Private**)।

| पोर्ट | कौन | प्रोटोकॉल |
|---|---|---|
| 47600 | होस्ट | TCP और UDP |
| 47610 | रिले | TCP, और तेज़ रास्ते के लिए UDP |

**Interact everywhere और Story co-op (प्रयोगात्मक, डिफ़ॉल्ट रूप से बंद)।** कहानी के इलाकों में गेम आगंतुकों को Spectator Mode में रखता है। **Interact everywhere** उन्हें वहाँ चालें और आइटम इस्तेमाल करने, उठाने, रखने और बनाने देता है। **Story co-op** उन्हें कहानी के पात्रों से बात करने देता है, और Interact everywhere चालू कर देता है। हर खिलाड़ी के पास वही दो स्विच होने चाहिए, वरना होस्ट सूची में नहीं आता। सिर्फ़ होस्ट का सेव कहानी की प्रगति रखता है।

Link Play अभी परीक्षण में है: जुड़ना और साथ घूमना काम करता है। लंबे सत्र, दो से ज़्यादा खिलाड़ी और इंटरनेट पर खेलना अभी टेस्ट नहीं हुए। यह कैसे काम करता है और `pkp.ini` की keys, [docs/multiplayer](../multiplayer/README.md) में देखें।

## आम समस्याएँ

- **पहली बार चलाने पर विंडो काली है, या गेम बहुत अटकता है।** गेम चलते समय कुछ शेडर बनाता है। लॉन्चर हर शुरुआत से पहले पहले देखे गए शेडर अनुवाद करता है और आपके बाहर निकलने के बाद फिर करता है, इसलिए शुरुआती रन खराब और बाद के बेहतर होते हैं। एक मिनट दें और खेलते रहें। अगर सेटअप बीच में रुक गया, तो **Game files** खोलें और जारी रखने के लिए **Set up** दबाएँ। यह पूरा हुआ काम रखता है।
- **लॉन्चर कहता है गेम फ़ाइलें पुराने वर्ज़न की हैं, या अपडेट नहीं मिला।** इस रिलीज़ को वर्ज़न 2.0.1a चाहिए। अपडेट इंस्टॉल करके गेम फिर से डंप करें। सिर्फ़ बेस गेम काफ़ी नहीं है।
- **लॉन्चर कहता है उसे कोई काम की गेम फ़ाइल नहीं मिली, या Set up कहता है कोई फ़ाइल मेल नहीं खाती।** वह फ़ोल्डर चुनें जिसमें `base` और `updates` हैं (जैसे `Pokopia_v2.0.1a`)। हर एक में `exefs` (`main`, `sdk`, `subsdk0`, `rtld`, `main.npdm`) और `romfs` होने चाहिए। जो फ़ाइल मेल नहीं खाती वह खराब है। उसे फिर डंप करें, या फिर से कॉपी करें।
- **लॉन्चर कहता है गेम डेटा (romfs) अधूरा है, या गेम शुरू होते ही "pkp.exe exited with code 4" के साथ बंद हो जाता है।** आपके डंपिंग टूल ने अपडेट का `romfs` बेस गेम की फ़ाइलों के बिना लिखा (अपडेट में सिर्फ़ बदली फ़ाइलें होती हैं)। बेस गेम को उसी डंप में रखें (`updates` के बगल में `base`, दोनों में `exefs` और `romfs`) और वह फ़ोल्डर चुनें जिसमें दोनों हों: लॉन्चर `base` से छूटी फ़ाइलें खुद जोड़ देता है। फिर एक बार और **Set up** दबाएँ। वरना गेम को अपडेट को बेस गेम में मिलाकर फिर डंप करें।
- **लॉन्चर कहता है डंप टेक्स्ट मोड में कॉपी हुआ।** आपने उसे FTP से टेक्स्ट मोड में कॉपी किया, जो `main`, `sdk` और `subsdk0` को खराब करता है। `exefs` और `romfs` फ़ोल्डर फिर से कॉपी करें और ट्रांसफ़र टाइप **Binary** रखें (FileZilla: Transfer, Transfer type, Binary)।
- **आपका डंप एक फ़ाइल है** (`.nsp`, `.xci`)। लॉन्चर गेम को फ़ोल्डरों के रूप में, या `.nup` या `.nx` के रूप में लेता है। अपने डंपिंग टूल से `.nsp` या `.xci` को फ़ोल्डरों में एक्सट्रैक्ट करें।
- **सेटअप किसी फ़ाइल के खुलने या हटने न पाने की गलती पर रुकता है।** फ़ोल्डर को किसी साधारण जगह रखें, जैसे `C:\Pokopia`, OneDrive के सिंक वाले फ़ोल्डर में नहीं। आपका एंटीवायरस बन रही फ़ाइलों को स्कैन कर रहा हो सकता है: इंतज़ार करें, या फ़ोल्डर को अनुमति दें। **Open setup log** दिखाता है कहाँ रुका।
- **Set up शुरू होने से पहले चेताता है।** यह पहले खाली डिस्क स्पेस (पैक की गई शेडर फ़ाइलों के साथ, जो डिफ़ॉल्ट है, लगभग 14 GB; अलग-अलग फ़ाइलों के साथ लगभग 31 GB), Vulkan, मेमोरी और प्रोसेसर जाँचता है। Intel 13वीं या 14वीं पीढ़ी के डेस्कटॉप CPU पर सेटअप से पहले BIOS अपडेट करें: Intel के फ़िक्स के बिना ये CPUs लंबे पूरे लोड में क्रैश हो सकते हैं। अगर बिल्ड के दौरान PC गरम हो या इस्तेमाल में कठिन लगे, तो **Use fewer cores** चुनें।
- **कम फ़्रेम रेट, या धुँधली तस्वीर।** तस्वीर का आकार गेम शुरू होने पर तय होता है। यह उस पल की विंडो या स्क्रीन का आकार है (**Settings**, **Display**)।
  - 4K स्क्रीन पर **Borderless** में शुरू करें तो गेम 4K पर बनता है। यह भारी है। **Render scale** घटाएँ।
  - **Windowed** (डिफ़ॉल्ट 1280x720) में शुरू करें और Alt+Enter दबाएँ, तो तस्वीर 720p ही रहती है, बड़ी करके। **Play** दबाने से पहले **Window mode** सेट करें।
  - **Frame rate limit** वह रखें जो आपकी स्क्रीन दिखा सके।
- **Link Play: आगंतुक होस्ट नहीं ढूँढ पाता।** **Visit by address** चुनें और होस्ट का पता टाइप करें (होस्ट का Link Play पेज इसे दिखाता है)। जाँचें कि होस्ट ने गेम शुरू किया और **Invite Others to Visit** चुना, Windows Firewall Private नेटवर्क पर `pkp.exe` को अनुमति देता है, और नेटवर्क Private है। कैफ़े, होटल और गेस्ट Wi-Fi जो डिवाइसों को अलग रखते हैं, सबके लिए LAN खेल रोकते हैं: रिले और रूम कोड के साथ **Internet** इस्तेमाल करें। इंटरनेट पर, रिले पता और रूम कोड जाँचें। Link Play पेज के बदलाव गेम के अगली बार शुरू होने पर लागू होते हैं।
- **Link Play: कनेक्शन अस्वीकार।** खिलाड़ी मेल नहीं खाते। सबको गेम 2.0.1, एक ही रिलीज़ (या क्रॉस-प्ले के लिए pokopia-native), और एक ही मॉड तथा वही **Interact everywhere** और **Story co-op** स्विच चाहिए।
- **Link Play: मिलने पर "Communication error" (9005)।** आप दोनों एक ही सेव की कॉपी खेल रहे हैं, इसलिए दोनों का player ID एक है। हर खिलाड़ी को अपना सेव चाहिए।
- **कंट्रोलर नहीं पहचाना जाता।** गेम में F1 दबाएँ और Controls पेज पर **Gamepad** पंक्ति देखें। जब गेम को कोई गेमपैड न दिखे तो वह "No gamepad open" कहता है। **Play** दबाने से पहले उसे जोड़ें, फिर दूसरा केबल या पोर्ट आज़माएँ। दूसरे प्रोग्राम बंद करें जो पैड रोके हों।

अगर इनमें से कुछ नहीं, तो [समस्याएँ रिपोर्ट करना](#reporting-problems) देखें।

## आपके सेव, लॉग और सेटिंग्स कहाँ हैं

ये सब गेम फ़ोल्डर में हैं: वह फ़ोल्डर जिसमें `PokopiaLauncher.exe` और `pkp.exe` हैं, जब तक आपने **Settings**, **Game folder** में दूसरा न चुना हो।

| क्या | कहाँ |
|---|---|
| सेव | `save` (**Settings** इसे कहीं और भेज सकता है: **Save folder**) |
| सेटिंग्स | `pkp.ini` और `controls.ini` (गेम में F1 `controls.ini` लिखता है) |
| लॉग | `pkp.log`, लॉन्चर से हर शुरुआत पर लिखा जाता है। क्रैश `pkp_crash.txt`, `pkp_freeze.txt` या `pkp_fatal.txt` भी लिखता है। |
| अनुवादित शेडर | `shader_cache` (लगभग 26 GB) |
| बिल्ड फ़ाइलें | `build` (सेटअप का काम का फ़ोल्डर) |
| खुला `.nup` या `.nx` | `nup_dump`, अगर आपने `.nup` या `.nx` चुना (इसके ऐड-ऑन, अगर हों, `nup_dump/aoc` में) |
| लॉन्चर सेटिंग्स | `%APPDATA%\org.pkp-recomp.launcher\launcher.json` (आपका डंप फ़ोल्डर और गेम फ़ोल्डर) |

किसी issue में गेम फ़ाइलें, `build` या `shader_cache` फ़ोल्डर, या सेव कभी न लगाएँ। `pkp.log` काफ़ी है।

## समुदाय

सवाल, बग से जुड़ी मदद, टेस्टिंग और चैट: [Recompia Discord](https://discord.recompia.com) से जुड़ें। लॉन्चर में **Settings > About > Community** के नीचे भी **Join Discord** बटन है। जिन बग को ठीक करना है, उन्हें अब भी [GitHub](https://github.com/featureJosh/pokopia-recomp/issues/new/choose) पर रिपोर्ट करें।

## समस्याएँ रिपोर्ट करना

[issue खोलें](https://github.com/featureJosh/pokopia-recomp/issues/new/choose) उस सत्र का `pkp.log`, अपनी GPU और अपना Windows वर्ज़न साथ दें (Linux पर: आपका डिस्ट्रिब्यूशन, Proton वर्ज़न और डिवाइस)।

**Linux, SteamOS और Linux हैंडहेल्ड:** मैं (Jerban) Linux की तरफ़ देखता हूँ, इसलिए Linux बग जल्दी ठीक होकर एक-क्लिक अपडेटर से भेजे जाते हैं। बार-बार अपडेट के लिए माफ़ी, और धैर्य के लिए धन्यवाद। Linux बग रिपोर्ट करते रहें, हमेशा अपने लॉग के साथ: `pkp.log`, और Set up की समस्याओं के लिए `setup.log` (देखें [आपके सेव, लॉग और सेटिंग्स कहाँ हैं](#where-your-saves-logs-and-settings-are))। Linux से बाहर की हर चीज़ की समीक्षा पहले Josh करते हैं।

## लाइसेंस

GPL-3.0. तीसरे पक्ष का कोड अपना लाइसेंस रखता है, देखें [CREDITS.md](../../CREDITS.md).
