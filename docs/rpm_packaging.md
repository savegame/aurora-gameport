# Упаковка RPM: имя пакета, rpath, подпись, валидация

Этот документ читай на **этапе 1** (rpath/patchelf при первой
сборке пакета) и на **этапах 5–6** (подпись и валидация перед
установкой на устройство и финальным RPM). Правила зависимостей и
`__requires_exclude` — в `docs/sfdk_build.md`; рецепты сборки
через sfdk — там же.

## Имя пакета и песочница

Пакет конечного приложения называется org.example.appname-1.0.0-3.aarch64.rpm,
где `org.example` — обратное доменное имя организации или разработчика,
`appname` — название приложения, `1.0.0` — версия (часть Version в spec-файле),
`3` — номер сборки (часть Release в spec-файле), `aarch64` — архитектура пакета.

На платформе Аврора пакет работает внутри песочницы и имеет следующие
ограничения:
- Все зависимости, которые собираются вместе с проектом, должны быть
  установлены вместе с пакетом в `/usr/share/org.example.appname/`, и далее,
  после сборки проекта, конечному бинарному файлу должен быть либо прописан
  rpath до этой папки, либо (проще) в секции `%install` spec-файла вызвать
  `patchelf` для добавления этого пути в пути поиска линковщика. Например,
  если собран проект mygame, установка выглядит так:
  ```
  install -m 0655 -D build/%{_arch}/mygame/mygame %{buildroot}%{_bindir}/%{name}
  patchelf --force-rpath --set-rpath %{_datadir}/%{name}/lib %{buildroot}%{_bindir}/%{name}
  ```
  По умолчанию patchelf вызывается в spec-файле, однако если в проекте
  собираются библиотеки, которые зависят друг от друга, их тоже стоит
  пропатчить, добавив правильный путь поиска линковки, по примеру выше.

Учти рантайм-эффект такой установки: приложение работает с симлинками
на файлы пакета, и несимметричная канонизация путей (realpath) ломает
строковые сравнения путей в движке — см. «Опасные места» в
`docs/render_pipeline_research.md`.

## Подпись пакета

После сборки первого RPM его нужно подписать ключами из SDK.
Подпись обязательна перед любой проверкой пакета: перед запуском
валидатора и перед установкой на устройство. Не подписываем пакет
ТОЛЬКО если пользователь явно попросил этого не делать.
- Ключи берём либо из SDK (папка `package-signing`: `regular_cert.pem`
  и `regular_key.pem`), либо скачиваем:
  https://developer.auroraos.ru/content-images/dev-documentation/regular_key.pem
  https://developer.auroraos.ru/content-images/dev-documentation/regular_cert.pem
  Кладём в корень проекта.
- Подписываем:
  ```
  sfdk engine exec sb2 -t <target_name>.default rpmsign-external sign -c /path/to/regular_cert.pem -k /path/to/regular_key.pem RPMS/org.example.appname-<version-release, может содержать git-хеш>.aarch64.rpm
  ```

## Валидация пакета (rpm-validator)

Далее пакет прогоняется через валидатор — так узнаём, есть ли
в зависимостях библиотеки, с которыми нельзя линковаться напрямую
(например, OpenAL):
```
sfdk engine exec sb2 -t <target_name>.default rpm-validator -p regular org.example.appname-<version-release, может содержать git-хеш>.aarch64.rpm
```
Если валидатор пишет, что библиотека (например, libopenal) не разрешена
как зависимость линковки — добавляем копирование этой библиотеки
из таргета в наш пакет, в `/usr/share/org.example.appname/lib`:
```
install -D %{_libdir}/libopenal.so.1* -t %{buildroot}%{_datadir}/%{name}/lib/
```
и, как описано в правиле «Исключение добавленных зависимостей из
валидации разрешённых библиотек» в `docs/sfdk_build.md`, добавляем
её имя в `__requires_exclude`.
