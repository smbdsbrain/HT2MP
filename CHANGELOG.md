# История изменений

Формат основан на [Keep a Changelog](https://keepachangelog.com/ru/1.1.0/), версии следуют [Semantic Versioning](https://semver.org/lang/ru/).

## [Unreleased]

## [0.1.0-alpha.1] — 2026-09-10

### Добавлено

- нативный Windows-лаунчер с изолированным staging Steam-копии;
- сетевой sidecar, безопасный x86 injector и проверяемый bridge;
- выделенный coordinator для Windows x86_64 и Linux x86_64;
- синхронизация движения, внешнего вида, колёс, света, повреждений, гудка, времени и погоды;
- сборка, тестирование, упаковка, secret scanning и provenance в GitHub Actions.

### Ограничения

- релиз имеет статус alpha и официально поддерживает только точную Steam-сборку;
- клиент отображает максимум семь удалённых машин;
- полная изоляция столкновений, gameplay-систем и сохранений ещё не завершена;
- Windows-бинарники не имеют Authenticode-подписи, сетевой трафик не шифруется.

[Unreleased]: https://github.com/smbdsbrain/HT2MP/compare/v0.1.0-alpha.1...HEAD
[0.1.0-alpha.1]: https://github.com/smbdsbrain/HT2MP/releases/tag/v0.1.0-alpha.1
