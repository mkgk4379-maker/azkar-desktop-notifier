# Azkar Desktop Notifier — إشعارات أذكار سطح المكتب

تطبيق خفيف لنظام Windows يعرض الأذكار دوريًا في إشعارات عائمة، ويمكن التحكم به من أيقونته بجوار الساعة. التطبيق مكتوب بلغة C باستخدام Win32 API وGDI.

## الميزات

- إضافة الأذكار وتعديلها وحذفها، مع حد يومي اختياري وعدّاد يُعاد ضبطه تلقائيًا كل يوم.
- ثمانية تصاميم للإشعارات وخمسة خيارات للحركة.
- اختيار موضع الإشعار وضبط الهوامش والخط والحجم ومدة العرض والفاصل بين الإشعارات.
- خيار التشغيل مع بدء Windows.
- حفظ الإعدادات في `azkar.ini` بجوار البرنامج؛ لا يتضمن هذا المستودع ملف إعدادات المستخدم.

## لقطات شاشة

أُضيفت الصور إلى [`docs/screenshots`](docs/screenshots/):

![لقطة شاشة 1](docs/screenshots/screenshot-01.png)

![لقطة شاشة 2](docs/screenshots/screenshot-02.png)

![لقطة شاشة 3](docs/screenshots/screenshot-03.png)

![لقطة شاشة 4](docs/screenshots/screenshot-04.png)

![لقطة شاشة 5](docs/screenshots/screenshot-05.png)

## التشغيل

لا يتضمن المستودع ملفًا تنفيذيًا جاهزًا. ابنِ البرنامج من المصدر وفق التعليمات أدناه، ثم شغّل `AzkarNotifier.exe` على Windows. ينشئ التطبيق ملف `azkar.ini` بجوار الملف التنفيذي عند التشغيل الأول.

## البناء

### Linux باستخدام MinGW-w64

على توزيعات Debian/Ubuntu، ثبّت الأدوات ثم ابنِ التطبيق:

```sh
sudo apt update
sudo apt install mingw-w64 python3
chmod +x build.sh
./build.sh
```

ينتج البناء ملف `AzkarNotifier.exe` لويندوز 64-بت. يعيد `build.sh` إنشاء `app.ico` فقط إذا لم يكن موجودًا.

### Windows باستخدام MinGW-w64 أو MSYS2

من بيئة shell تتوفر فيها `gcc` و`windres`:

```sh
windres -c 65001 -i app.rc -o app_res.o
gcc -std=c11 -Os -s -municode -mwindows -DUNICODE -D_UNICODE \
  -o AzkarNotifier.exe main.c app_res.o \
  -lgdi32 -luser32 -lshell32 -ladvapi32 -lcomctl32
```

## الخطوط الاختيارية

يعمل التطبيق بخط Windows الافتراضي عند عدم وجود الخطوط الإضافية. يمكن وضع ملفات الخطوط التي تملك حق استخدامها داخل مجلد `fonts` بجوار البرنامج؛ لا تُضمّن ملفات خطوط طرف ثالث في هذا المستودع، ويجب التحقق من ترخيص كل خط قبل توزيعه.

## محتويات المستودع

يتضمن المستودع الشيفرة وملفات الموارد وسكربت البناء وتوليد الأيقونة. تُستبعد ملفات البناء والملف التنفيذي وملف إعدادات المستخدم عبر `.gitignore`.

## الترخيص

هذا المشروع مرخّص بموجب [MIT License](LICENSE).
