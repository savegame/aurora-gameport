# Акселерометр и гироскоп на ОС Аврора

Как читать показания акселерометра и гироскопа на ОС Аврора напрямую
у демона `sensorfwd`: протокол и готовый пример на C.

Готовый пример лежит в `examples/sensors/` — его можно брать
и адаптировать под свой движок (см. «Пример кода» ниже).

## Как это работает

Датчиками движения на ОС Аврора управляет демон **sensorfwd**
(пакет `sensorfw-qt5`, апстрим
[github.com/sailfishos/sensorfw](https://github.com/sailfishos/sensorfw)).
На устройствах, построенных поверх libhybris (мост к Android HAL),
sensorfwd получает данные через hybris-адаптеры — для клиента это не
имеет значения, важно только то, как клиент говорит с sensorfwd.

Клиент общается с sensorfwd по двум каналам одновременно:

- **D-Bus** (system bus, сервис `com.nokia.SensorService`) —
  управление сессией: создание, старт/стоп, интервал опроса.
- **Unix-сокет** `/run/sensord.sock` — сам поток измерений (по
  соображениям производительности данные не гоняют через D-Bus).

## Управление сессией (D-Bus)

`id` во всех вызовах ниже — логическое имя сенсора внутри sensorfwd
(`accelerometersensor`, `gyroscopesensor` и т.п.), одна и та же
строка используется и в `loadPlugin`/`requestSensor`/`releaseSensor`,
и как суффикс объекта сенсора (`/SensorManager/<id>`, см. ниже). Это
не придуманное клиентом имя — его нужно взять из списка, который
знает сам sensorfwd:

```
dbus-send --system --print-reply --dest=com.nokia.SensorService \
  /SensorManager local.SensorManager.availableSensorPlugins
```

Метод возвращает массив строк — все `id`, доступные на данном
устройстве (обычно среди них `accelerometersensor`, `alssensor`,
`compasssensor`, `gyroscopesensor`, `magnetometersensor`,
`orientationsensor`, `pressuresensor`, `proximitysensor`,
`rotationsensor` — но состав зависит от железа, поэтому источник
истины — сам вызов, а не этот список).

```
/SensorManager   local.SensorManager
    availableSensorPlugins() -> as              (список id, см. выше)
    loadPlugin(s: id) -> b
    requestSensor(s: id, x: pid) -> i sessionId
    releaseSensor(s: id, i: sessionId, x: pid) -> b
```

**Важно:** `loadPlugin(id)` нужно вызвать раньше `requestSensor` —
иначе сессия не создаётся, с ошибкой вида `requested sensor id
'...' not registered`.

После `loadPlugin(id)` появляется D-Bus объект этого сенсора,
`/SensorManager/<id>` — тот же `id`, что и в вызове:

```
/SensorManager/accelerometersensor   local.AccelerometerSensor
/SensorManager/gyroscopesensor       local.GyroscopeSensor
    start(i: sessionId)
    stop(i: sessionId)
    setInterval(i: sessionId, i: value_ms)
```

Для других сенсоров (`magnetometersensor`, `orientationsensor`,
`rotationsensor`, ...) имя интерфейса следует той же схеме
(`local.<Type>Sensor`), но перед использованием его стоит
подтвердить интроспекцией на целевом устройстве:

```
dbus-send --system --print-reply --dest=com.nokia.SensorService \
  /SensorManager/<id> org.freedesktop.DBus.Introspectable.Introspect
```
(объект появляется только после `loadPlugin` для этого `id`).

## Поток данных (unix-сокет)

`/run/sensord.sock` — обычный файловый AF_UNIX SOCK_STREAM сокет.
Хендшейк перед получением данных:

1. `connect()`.
2. Сервер сразу после `accept()` пишет 1 tag-байт.
3. Клиент пишет 4 сырых байта — `sessionId` как нативный `int32`.
4. Клиент читает 1 байт (потребляет tag) — готово, можно вызывать
   `start(sessionId)` по D-Bus.

Дальше в сокет пишутся кадры (по одному семплу или пачкой, если
включена буферизация):

```
[uint32 count][count * размер_элемента байт]
```

## Формат элемента данных

Апстримные исходники sensorfw (`datatypes/genericdata.h`) описывают
элемент кадра как `TimedXyzData { quint64 timestamp_; float x_, y_,
z_; }` — 20 «сырых» байт, паддинг до 24 из-за выравнивания
`uint64_t`.

**На практике клиент, собранный строго по этому описанию, не смог
стабильно разобрать поток**: второй и все последующие семплы в
пачке (sensorfwd регулярно шлёт больше одного семпла за одну
операцию записи) съезжали на 4 байта, потому что реальный элемент
кадра устроен иначе. Проверка сырым дампом байт с работающего
устройства (см. `sensorfw_rawdump.c` ниже) показывает, что все 24
байта заняты данными — это четыре поля `int32`, а не три `float` с
хвостовым паддингом:

```c
struct sensorfw_wire_xyz_t {
    uint64_t timestamp_us; /* монотонное время демона, микросекунды */
    int32_t  x, y, z;      /* сырые единицы драйвера, НЕ float, НЕ SI */
    int32_t  reserved;     /* константа на сессию/тип сенсора, смысл не выяснен */
};
```

Пример сырых байт одного кадра (устройство лежит неподвижно,
акселерометр):
```
0b 19 5c b2 67 00 00 00  0f 00 00 00  f0 ff ff ff  ed 03 00 00  7e 00 00 00
└──── timestamp (u64) ──┘ └──x=15──┘  └──y=-16──┘  └─z=1005──┘  └─?=126──┘
```
`z≈1005` при неподвижно лежащем устройстве соответствует ~1 g в
единицах milli-g — рабочая гипотеза по масштабу акселерометра, не
проверенная на других осях/ориентациях. Поле `reserved` не паддинг
(все 24 байта реальные), но и не документировано; в наблюдениях оно
не менялось от кадра к кадру в рамках одной сессии и отличалось
между типами сенсоров — похоже на status/range/accuracy слово, а не
per-sample данные. У акселерометра и гироскопа структура кадра
совпадает по размеру и раскладке.

D-Bus-свойство/метод `xyz` на объекте сенсора (и сигнал
`dataAvailable`) кодирует значение отдельно и по-другому —
`(tiii)`, то есть timestamp + три `int32`. Это самостоятельный,
«разовый» канал получения последнего значения через D-Bus; для
потокового чтения он не нужен — поток всегда идёт через сокет.

**Перед использованием на новом устройстве/сборке sensorfw раскладку
стоит перепроверить сырым дампом (`sensorfw_rawdump.c`), а не
доверять слепо описанию из апстримных исходников** — как показано
выше, оно может расходиться с реальным потоком байт.

## Разрешение в .desktop-файле

Приложение, запущенное в песочнице (Sailjail — штатный режим для
пакетов из Aurora Store), по умолчанию не имеет доступа ни к
`com.nokia.SensorService` на D-Bus, ни к `/run/sensord.sock`. Чтобы
получить доступ, в `.desktop`-файл приложения нужно добавить
разрешение `Sensors`:

```
X-Sailjail-Permissions=Sensors;...
```
(в списке через `;`, вместе с остальными нужными приложению
разрешениями).

Это разрешение — общая специфика песочницы Аврора/Sailfish и не
зависит от конкретного устройства или сборки sensorfw: без него
`requestSensor`/`start` из sandboxed-приложения не пройдут, даже
если протокол реализован верно.

## Пример кода (`examples/sensors/`)

- `sensorfw_client.h` / `sensorfw_client.c` — рабочий модуль на
  чистом C, реализует протокол выше. Зависимость — `libdbus-1` (для
  сессии), сам сокет — обычный POSIX.

  `sensorfw_client_open()` принимает не произвольную строку, а
  значение `enum sensorfw_sensor_type_t` — набор сенсоров, для
  которых и D-Bus интерфейс, и раскладка данных из «Формата элемента
  данных» выше подтверждены на реальном железе. Так опечатка в `id`
  или попытка открыть сенсор с ещё не проверенной раскладкой не
  проходят на этапе компиляции/вызова, а не всплывают позже как
  «странные» данные:

  ```c
  typedef enum {
      SENSORFW_SENSOR_ACCELEROMETER,
      SENSORFW_SENSOR_GYROSCOPE,
      SENSORFW_SENSOR_TYPE_COUNT
  } sensorfw_sensor_type_t;

  const char *sensorfw_sensor_type_name( sensorfw_sensor_type_t type );
  bool sensorfw_sensor_type_from_name( const char *name, sensorfw_sensor_type_t *out );

  /* какие из типов выше реально есть на этом устройстве */
  int sensorfw_client_list_available( sensorfw_sensor_type_t *out, int max_types );

  sensorfw_client_t *sensorfw_client_open( sensorfw_sensor_type_t type );
  void sensorfw_client_close( sensorfw_client_t *client );
  bool sensorfw_client_set_interval_ms( sensorfw_client_t *client, int interval_ms );
  bool sensorfw_client_start( sensorfw_client_t *client );
  bool sensorfw_client_stop( sensorfw_client_t *client );
  int  sensorfw_client_fd( const sensorfw_client_t *client );
  bool sensorfw_client_pump( sensorfw_client_t *client, sensorfw_sample_cb cb, void *user_data );
  ```

  Типичный цикл: сначала узнать, что реально есть на устройстве,
  затем открыть нужный тип (как `mce_keepalive_pump()` — `pump()`
  дёргать раз в кадр):

  ```c
  sensorfw_sensor_type_t available[SENSORFW_SENSOR_TYPE_COUNT];
  int n = sensorfw_client_list_available( available, SENSORFW_SENSOR_TYPE_COUNT );
  /* найти SENSORFW_SENSOR_GYROSCOPE среди available[0..n) перед открытием,
   * если он не гарантирован на всех целевых устройствах */

  sensorfw_client_t *accel = sensorfw_client_open( SENSORFW_SENSOR_ACCELEROMETER );
  sensorfw_client_set_interval_ms( accel, 20 ); /* ~50 Hz */
  sensorfw_client_start( accel );
  /* в главном цикле: */
  sensorfw_client_pump( accel, on_sample, NULL );
  /* на закрытии: */
  sensorfw_client_close( accel );
  ```

  `sensorfw_client_pump()` неблокирующий: сокет переводится в
  `O_NONBLOCK` сразу после хендшейка, кадр разбирается по частям
  между вызовами — безопасно дёргать каждый кадр игрового цикла.

  Отдельно, только для расширения набора поддерживаемых типов —
  диагностический путь в обход enum: `sensorfw_client_list_plugins_raw()`
  (полный список id, которые знает демон на этом устройстве, без
  фильтрации по `sensorfw_sensor_type_t`) и `sensorfw_client_open_raw(id)`
  (открыть по произвольному id, с угадыванием имени D-Bus интерфейса
  по конвенции). У результата `sensorfw_client_open_raw()` раскладка
  данных для `sensorfw_client_pump()` не гарантирована — использовать
  вместе с `sensorfw_rawdump.c`, не для реального чтения показаний.

- `sensorfw_demo.c` и `sensorfw_rawdump.c` — не рабочий код, а
  диагностические утилиты **на крайний случай**: если на конкретном
  устройстве показания не сходятся с описанным выше протоколом
  (шум, `NaN`, десинки кадров и т.п.). `sensorfw_demo` печатает
  распарсенные показания сенсора в stdout, `sensorfw_rawdump` —
  сырые байты кадров без интерпретации, чтобы свериться с реальной
  раскладкой на этом устройстве (`sensorfw_rawdump --list` — вывести
  все id, которые демон знает на этом устройстве). Использовать их
  для отладки —
  собрать под целевую архитектуру и **выполнить бинарник на
  устройстве только после явного одобрения пользователя**: на
  устройствах с контролем целостности исполняемых файлов
  неподписанный бинарник не запустится вообще, потребуется подпись
  ключом устройства — процедура и доступ к ключу зависят от
  конкретного устройства и должны быть согласованы отдельно.

## Сборка

```
pkg-config --cflags --libs dbus-1
```

В spec-файл пакета:
```
BuildRequires: pkgconfig(dbus-1)
```
