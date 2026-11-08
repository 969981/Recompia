# source-sha256: bbc8b97f7c219b7daebac3445655f85afe641aecf5d6b34e5de70e2a51bd8bf3
هذه ترجمة آلية لملف README الإنجليزي، وقد تحتوي على أخطاء. النسخة الإنجليزية هي المرجع.

Pokopia PC @VERSION@ لنظام Linux وSteamOS وSteam Deck

1. في Steam، ثبّت Proton Experimental (المكتبة، الأدوات؛ على Steam Deck بدّل إلى وضع سطح المكتب أولاً).
2. شغّل PokopiaLauncher.AppImage واختر مجلد نسختك المستخرجة (base/ و updates/<version>/، ولكل منهما exefs/ و romfs/) أو ملف .nup. الإصدار 2.0.1a مطلوب.
3. اضغط Set up. في المرة الأولى ينزّل أدوات البناء (Python وZig، نحو 100 ميغابايت)، ويبني pkp.exe وذاكرة الشيدرات على هذا الحاسوب، عبر Proton. يمكنك الإيقاف المؤقت والمتابعة لاحقاً.
4. اضغط Play. تعمل اللعبة عبر Proton من Steam.

إذا لم يبدأ PokopiaLauncher.AppImage (نظام بدون FUSE): ./PokopiaLauncher.AppImage --appimage-extract-and-run
من Steam (وضع الألعاب على Deck): أضف PokopiaLauncher.AppImage كلعبة غير تابعة لـ Steam مع خيارات التشغيل --play.

التحديث: استخرج الإصدار الجديد فوق تثبيتك. إذا غيّر التحديث شيفرة اللعبة، يخبرك المشغّل: اضغط Set up مرة أخرى وسيعيد بناء pkp.exe. تبقى شيدراتك وملفات حفظك.

المجتمع والمساعدة: https://discord.gg/qE656Fed5N

التراخيص: LICENSE.txt و licenses/.
