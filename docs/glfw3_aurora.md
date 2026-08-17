# GLFW3 под ОС Аврора (форк с фиксами)

Этот документ читай, только если портируемый проект использует GLFW3
(окно и ввод через GLFW). Здесь собрано: какую версию GLFW3 брать, как
собрать её в пакете, и GLFW-специфичные API, нужные на этапах поворота
контента (ориентация дисплея, buffer transform).

## Какую версию брать

Использовать ТОЛЬКО эту версию GLFW3 — в ней фиксы под ОС Аврора:

- https://github.com/savegame/glfw (ветка `3.4-wl-shell` — в ней
  события ориентации дисплея и `glfwSetWindowContentTransform`)

Репозиторий выкачивается как подмодуль в папку `glfw3`.

## Сборка в пакете

1. В spec-файле добавить следующие `BuildRequires`:

```
BuildRequires: pkgconfig(wayland-client)
BuildRequires: pkgconfig(wayland-cursor)
BuildRequires: pkgconfig(wayland-egl)
BuildRequires: pkgconfig(wayland-protocols)
BuildRequires: pkgconfig(wayland-scanner)
BuildRequires: pkgconfig(glesv2)
BuildRequires: pkgconfig(xkbcommon)
BuildRequires: pkgconfig(vulkan)
BuildRequires: pkgconfig(egl)
```

2. В секции `%build`, перед сборкой основного проекта, добавить скрипт:

```
mkdir -p build/%{_arch}/glfw
cmake \
    -G Ninja \
    -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
    -DGLFW_LIBRARY_TYPE=STATIC \
    -DCMAKE_BUILD_TYPE=Release \
    -DGLFW_BUILD_WAYLAND=ON \
    -DGLFW_BUILD_X11=OFF \
    -DGLFW_STANDALONE=OFF \
    -DGLFW_AURORAOS=ON \
    -DGLFW_BUILD_EXAMPLES=OFF \
    -DGLFW_BUILD_TESTS=OFF \
    -DGLFW_BUILD_DOCS=OFF \
    -DCMAKE_INSTALL_PREFIX=build/%{_arch}/glfw.install \
    -Bbuild/%{_arch}/glfw \
    -Sglfw3
cmake --build build/%{_arch}/glfw
cmake --install build/%{_arch}/glfw
```

glfw удобно собрать статикой, но можно и динамикой. Далее в приложении
нужно указать пути для линковки с glfw, например вызвать cmake через
env:

```
env CMAKE_PREFIX_PATH=build/%{_arch}/glfw.install:$CMAKE_PREFIX_PATH \
cmake <флаги cmake>
```

или другим способом (исправить скрипты сборки проекта, если это
доступно).

## API ориентации дисплея

В модифицированном под Аврору GLFW (ветка `3.4-wl-shell`) поддержка
ориентации есть — используй её:

- enum `GLFW_TRANSFORM_NORMAL` / `_90` / `_180` / `_270` (значения 0–3,
  совпадают с `wl_output_transform`) и FLIPPED-варианты (4–7);
- `glfwGetMonitorTransform(monitor)` — текущий transform монитора;
- событие `GLFW_TRANSFORM_CHANGED` через `glfwSetMonitorCallback`
  (приходит в том же колбэке, что `GLFW_CONNECTED` /
  `GLFW_DISCONNECTED`);
- `glfwSetWindowContentTransform(window, transform)` — готовая
  обёртка над `wl_surface_set_buffer_transform`: выставляет buffer
  transform без ручной линковки wayland-client (используй её вместо
  ручного вызова `wl_surface_set_buffer_transform` — сниппет из
  `docs/sdl2_aurora.md` для SDL2, здесь он не нужен).

Если используется этот GLFW — НЕ интерпретируй `wl_output::transform`
вручную, бери значения из GLFW API.

Тип панели (портретная/ландшафтная) определяется по нативному
разрешению текущего монитора — один раз при инициализации и далее
только при смене монитора (перенос окна на другой output) по событию
фреймворка. Одно и то же значение transform идёт и в поворот квада
(`set_rotation` FBO-модуля), и в `glfwSetWindowContentTransform` —
см. «Требования к FBO-модулю» и «Маппинг ориентаций под ориентацию
игры» в `docs/fbo_module.md`.

## Сворачивание приложения

Для модуля запрета гашения экрана (MCE) событием сворачивания в GLFW
является иконификация окна — см. `docs/mce_display_blanking.md`.
