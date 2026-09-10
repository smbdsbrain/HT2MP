# Участие в разработке

Для небольшого исправления можно сразу открыть pull request. Крупную идею лучше сначала обсудить в [Discussions](https://github.com/smbdsbrain/HT2MP/discussions). Проверьте, нет ли уже похожей задачи, и старайтесь посвящать один pull request одному изменению.

Если нужна помощь с запуском игры, начните с [поддержки](SUPPORT.md).

## Сборка клиента для Windows

Нужны Git, CMake 3.25+, Visual Studio 2022 с компонентом **Desktop development with C++** и Windows SDK. При первой настройке требуется интернет для загрузки зависимостей. Команды выполняются из корня репозитория:

```powershell
cmake --preset windows-x86
cmake --build --preset build-x86 --parallel
ctest --preset test-x86
```

Игра и модуль HT2MP — 32-разрядные, поэтому клиент собирается с профилем `windows-x86`, в том числе на Windows x64. Чтобы получить готовый архив:

```powershell
cmake --build --preset package-client-windows-x86
```

## Сборка сервера

Для Windows нужны те же инструменты, что и для клиента:

```powershell
.\tools\Build-Ht2mpServer.ps1 -Platform windows-x64
```

Для Linux нужны Git, CMake 3.25+, компилятор с поддержкой C++20 и Ninja:

```bash
bash tools/build-server.sh
```

Архивы сервера появятся в `dist/server/`. Для сборки и тестов в Linux без упаковки:

```bash
cmake --preset server-linux-x64
cmake --build --preset build-server-linux-x64 --parallel
ctest --preset test-server-linux-x64
```

Устройство компонентов описано в [техническом обзоре](docs/OVERVIEW.md), запуск готового сервера — в [руководстве сервера](docs/SERVER.md).

## Перед отправкой изменений

Опишите, что изменилось и как вы это проверили. Для изменений кода запустите соответствующие тесты из команд выше. Перед pull request также выполните:

```powershell
pwsh tools/audit-public-tree.ps1
git diff --check
```

Не добавляйте файлы игры, личные сохранения, логи, дампы, скриншоты и секреты. Единственное исключение для сохранений — анонимизированный `apps/launcher/assets/HT2MP.pl1`. В примерах заменяйте токены, адреса и личные пути вымышленными значениями. Не присылайте декомпилированные игровые файлы или материалы, на распространение которых у вас нет прав.

Авторский вклад принимается под WTFPL v2. Для сторонних материалов нужно сохранить их лицензии и указание авторства.
