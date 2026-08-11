# Экранная клавиатура Maliit на ОС Аврора

Как вызвать встроенную экранную клавиатуру (OSK) из нативного
приложения на ОС Аврора: протокол, последовательность вызовов и
готовый пример на C.

Рабочий пример лежит в `examples/maliit/` — его можно брать
и адаптировать под свой движок (см. «Пример кода» ниже).

## Общая схема

На ОС Аврора (как и на Sailfish OS) экранной клавиатурой управляет
**maliit-server**. Приложение общается с ним через DBus, но не через
session bus напрямую, а через **peer-to-peer DBus-соединение**,
адрес которого нужно сначала узнать.

В обмене участвуют два DBus-интерфейса:

- `com.meego.inputmethod.uiserver1` (объект
  `/com/meego/inputmethod/uiserver1`) — методы, которые **приложение
  вызывает на сервере**: `activateContext`, `updateWidgetInformation`,
  `showInputMethod`, `hideInputMethod`, `reset`.
- `com.meego.inputmethod.inputcontext1` (объект
  `/com/meego/inputmethod/inputcontext`) — объект, который
  **экспонирует само приложение**. Через него сервер присылает
  события обратно: `commitString` (введённый текст), `updatePreedit`
  (набираемый текст), `keyEvent` (служебные клавиши),
  `updateInputMethodArea` (область экрана, занятая клавиатурой),
  `imInitiatedHide` / `activationLostEvent` (клавиатура скрыта
  системой).

## Discovery: как узнать адрес сервера

1. Если выставлена env-переменная `MALIIT_SERVER_ADDRESS` —
   используем её значение как есть.
2. Иначе спрашиваем у session bus свойство `address` интерфейса
   `org.maliit.Server.Address` на сервисе `org.maliit.server`,
   объект `/org/maliit/server/address` (через стандартный
   `org.freedesktop.DBus.Properties.Get`).
3. Открываем peer-to-peer соединение по полученному адресу
   (`G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT` в GDBus).
   При вызовах методов на таком соединении destination не
   указывается (NULL).

## Последовательность показа клавиатуры

1. **Инициализация (один раз на старте приложения):** подключиться
   к серверу и зарегистрировать свой объект input context
   (`/com/meego/inputmethod/inputcontext`) с introspection XML —
   через него сервер будет присылать события.
2. **focus_in** — поле ввода получило фокус. Вызвать
   `activateContext`, затем `updateWidgetInformation` со словарём
   состояния поля (`a{sv}`): `focusState=true`, `contentType`
   (текст/число/e-mail/URL), `predictionEnabled`,
   `correctionEnabled`, `autocapitalizationEnabled`, `hiddenText`
   (пароль), `cursorRectangle` и др. Без focus-in сервер текст
   не присылает.
3. **show** — вызвать `showInputMethod`. Клавиатура появляется
   на экране.
4. **Ввод:** сервер присылает события в наш input context:
   - печатные символы — через `commitString` (UTF-8 строка);
   - служебные клавиши (Backspace, Enter, стрелки, Home/End, Delete,
     Tab, Escape) — через `keyEvent` с кодами клавиш в нумерации
     `Qt::Key`;
   - `updatePreedit` — промежуточный (компонируемый) текст;
   - `updateInputMethodArea` — прямоугольник экрана, занятый
     клавиатурой (можно использовать, чтобы UI не прятался под
     клавиатуру).
5. **hide** — вызвать `hideInputMethod` (и `focus_out` —
   `updateWidgetInformation` с `focusState=false`), когда ввод
   завершён.

## Ориентация контента

Сервер клавиатуры должен знать текущую ориентацию контента
приложения — иначе клавиатура откроется не в той ориентации.
Для этого на интерфейсе `uiserver1` есть два вызова:

- `appOrientationAboutToChange(int32)` — предупреждение о смене;
- `appOrientationChanged(int32)` — фактическая смена.

Аргумент — угол в градусах (0/90/180/270). Вызывать **вместе с
`wl_surface_set_buffer_transform`** на каждой смене ориентации
(см. «Этап 3. Поворот и ресайз контента» в AGENTS.MD), а также
при показе клавиатуры. Маппинг из `SDL_DisplayOrientation`
(как в платформенной SDL2 ОС Аврора):

| SDL_DisplayOrientation | Градусы |
|---|---|
| `SDL_ORIENTATION_PORTRAIT` | 0 |
| `SDL_ORIENTATION_PORTRAIT_FLIPPED` | 180 |
| `SDL_ORIENTATION_LANDSCAPE` | 270 |
| `SDL_ORIENTATION_LANDSCAPE_FLIPPED` | 90 |

В обёртке это один вызов — `maliit_client_set_orientation(angle)`,
он отправляет оба метода подряд.

## Тонкости

- **GLib main context.** Реализация на GDBus доставляет входящие
  вызовы через главный контекст GLib — его нужно регулярно
  прокручивать из того же потока, где выполнялась инициализация
  (например, раз в кадр, неблокирующе:
  `g_main_context_pending` + `g_main_context_iteration`).
- **Держите соединение живым** всё время работы приложения:
  частые переподключения/активации заставляют maliit-server
  «моргать» клавиатуру. Достаточно focus_in/focus_out и show/hide.
- **Точка вызова клавиатуры.** Привязывайте show/hide к запросу
  текстового ввода:
  - если движок имеет собственные методы «начать/закончить ввод
    текста» — вызывайте клавиатуру из них;
  - если ввод идёт через фреймворк — используйте его механизмы:
    в SDL2 это `SDL_StartTextInput` / `SDL_StopTextInput`
    (плюс контроль через `SDL_IsTextInputActive`), в GLFW —
    соответствующие события/запросы ввода текста.
- **Парольные поля:** `hiddenText=true`, prediction/correction/
  autocaps выключить.

## Пример кода (`examples/maliit/`)

- `maliit_client.h` / `maliit_client.c` — переносимая обёртка над
  Maliit. Наружу — чистый C (структура колбэков
  `maliit_callbacks_t`: `commit_string`, `preedit_string`,
  `key_event`, `visibility`, `area_changed`), внутри — GLib/GDBus.
  Никакой привязки к движку, файлы можно копировать в проект как
  есть. Публичный API: `maliit_client_init/shutdown/pump`,
  `maliit_client_show/hide`, `maliit_client_focus_in/focus_out`,
  `maliit_client_set_orientation` (см. «Ориентация контента»),
  настройки контента (`maliit_client_set_content_type`,
  `maliit_client_set_password_mode` и др.).
- `maliit_bridge.h` / `maliit_bridge.c` — пример моста обёртки
  в SDL2: `commit_string` превращается в `SDL_TEXTINPUT`
  (строка режется на куски по 31 байту — размер буфера
  `SDL_TextInputEvent.text` — с аккуратным учётом границ
  многобайтных UTF-8 символов), `keyEvent` — в
  `SDL_KEYDOWN`/`SDL_KEYUP` через маппинг `Qt::Key → SDL_Scancode`.
  В результате движок видит обычные события ввода SDL2 и не знает
  про существование Maliit. Для другого фреймворка/движка мост
  переписывается под его события ввода, `maliit_client.*` при этом
  не меняется.

Типичный цикл использования:

```c
maliit_callbacks_t cb = {0};
cb.commit_string = on_commit;   // -> события ввода движка
cb.key_event     = on_key;
maliit_client_init(&cb);
// в главном цикле, раз в кадр:
maliit_client_pump();
// когда движок запросил ввод текста:
maliit_client_focus_in();
maliit_client_show();
// когда ввод завершён:
maliit_client_hide();
maliit_client_focus_out();
// на выходе:
maliit_client_shutdown();
```

## Сборка

Обёртка зависит от GLib/GIO. Для сборки нужны:

```
pkg-config --cflags --libs glib-2.0 gio-2.0 gio-unix-2.0
```

В spec-файл пакета добавить:

```
BuildRequires: pkgconfig(glib-2.0)
BuildRequires: pkgconfig(gio-2.0)
BuildRequires: pkgconfig(gio-unix-2.0)
```
