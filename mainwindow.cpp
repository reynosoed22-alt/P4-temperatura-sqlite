#include "mainwindow.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QSlider>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QStyle>
#include <QFontInfo>
#include <QJsonDocument>
#include <QJsonValue>
#include <QDateTime>
#include <QChart>
#include <QChartView>
#include <QLineSeries>
#include <QValueAxis>
// Práctica 4
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QStandardPaths>
#include <QDir>
#include <QFile>

// Contrato con el firmware de la Práctica 2 (practica4.ino de 2026A, puerto 81):
//   Qt -> ESP32  {"tipo":"latido"}
//                {"tipo":"mover","accion":"avanzar|retroceder|izquierda|derecha|parar","ms":400}
//                {"tipo":"servo","angulo":60..120}  (límite del firmware P2-1.1.0)
//                {"tipo":"leer_distancia"}
//                {"tipo":"scan","angulo":60..120}
//   ESP32 -> Qt  saludo, latido_ok, mover_ack / mover_err, mover_inicio, mover_fin,
//                {"tipo":"distancia_lectura","d":cm}, {"tipo":"scan","d":cm,"angulo":n}, telemetria
// El firmware reemplaza el movimiento en curso cuando llega una orden nueva, así que
// repetir "mover" mientras el dedo está en el botón no acumula órdenes.

static const int MS_MOVIMIENTO = 400;   // duración de cada orden "mover"
static const int MS_REPETIR = 200;      // cada cuánto se reenvía mientras se presiona
static const int VENTANA_MUESTRAS = 40; // puntos visibles en la gráfica
// Rango que acepta el firmware P2-1.1.0 (robot_logic.h: SERVO_MIN/SERVO_MAX)
static const int SERVO_MIN = 60;
static const int SERVO_MAX = 120;
static const int PASO_BARRIDO = 15;
// Extra P3: límites de la ruta grabada
static const int MAX_PASOS = 40;        // pasos que caben en una ruta
static const int MAX_PAUSA_MS = 3000;   // pausas más largas se recortan al repetir
static const int MIN_PASO_MS = 100;     // toques más cortos se ignoran

// Regla antiplagio de la Práctica 4: nombre · código · temperatura_XXXX.db
// (XXXX = últimos 4 dígitos del código). Debe verse en todas tus capturas.
static const char *ALUMNO = "Edgar Ulises Reynoso Gonzalez · 223823098 · temperatura_3098.db";
static const char *ARCHIVO_BASE = "temperatura_3098.db";
// Debe coincidir con SIMULAR_DHT del firmware: "simulada" (true) o "dht22" (false)
static const char *FUENTE = "simulada";
static const int MS_GUARDAR_AUTO = 10000;   // DHT22 real: nunca más rápido que 2 s

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    construirInterfaz();

    connect(&m_socket, &QWebSocket::connected, this, &MainWindow::alConectar);
    connect(&m_socket, &QWebSocket::disconnected, this, &MainWindow::alDesconectar);
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &MainWindow::alRecibirMensaje);
    connect(&m_socket, &QWebSocket::errorOccurred, this, &MainWindow::alOcurrirError);

    m_latido.setInterval(1000);
    connect(&m_latido, &QTimer::timeout, this, [this] { enviar({{"tipo", "latido"}}); });

    m_repetir.setInterval(MS_REPETIR);
    connect(&m_repetir, &QTimer::timeout, this, &MainWindow::repetirMovimiento);

    m_servo.setInterval(60);
    m_servo.setSingleShot(true);
    connect(&m_servo, &QTimer::timeout, this, &MainWindow::enviarServoPendiente);

    m_medirAuto.setInterval(500);
    connect(&m_medirAuto, &QTimer::timeout, this, &MainWindow::alPresionarMedir);

    m_barrido.setInterval(700);   // el firmware espera 500 ms antes de medir en cada "scan"
    connect(&m_barrido, &QTimer::timeout, this, &MainWindow::siguientePasoBarrido);

    m_ruta.setSingleShot(true);
    connect(&m_ruta, &QTimer::timeout, this, &MainWindow::siguientePasoRuta);

    // P4 · E3: lectura automática cada 10 s mientras «Guardar cada 10 s» esté marcado
    m_leerTempAuto.setInterval(MS_GUARDAR_AUTO);
    connect(&m_leerTempAuto, &QTimer::timeout, this, [this] { enviar({{"tipo", "leer_dht"}}); });

    // P4 · E2: al final, para que un error de la base no impida conectar el robot
    abrirBase();
}

// ---------------------------------------------------------------- interfaz
void MainWindow::construirInterfaz()
{
    auto *central = new QWidget(this);
    auto *raiz = new QVBoxLayout(central);
    raiz->setSpacing(8);

    auto *alumno = new QLabel(QString::fromUtf8(ALUMNO));
    alumno->setAlignment(Qt::AlignCenter);
    alumno->setWordWrap(true);   // P4: el nombre y la base deben verse completos en el teléfono
    alumno->setStyleSheet("font-weight: bold; color: #1F4E79;");
    raiz->addWidget(alumno);

    // 1) Conexión
    auto *cajaConexion = new QGroupBox(tr("Conexión con el robot"));
    auto *filaConexion = new QHBoxLayout(cajaConexion);
    m_ip = new QLineEdit(QStringLiteral("192.168.100.4"));   // IP del robot en mi red
    m_ip->setPlaceholderText(tr("IP de la ESP32-S3"));
    m_ip->setInputMethodHints(Qt::ImhPreferNumbers);   // teclado numérico en Android
    m_puerto = new QSpinBox;
    m_puerto->setRange(1, 65535);
    m_puerto->setValue(81);
    m_conectar = new QPushButton(tr("Conectar"));
    filaConexion->addWidget(m_ip, 3);
    filaConexion->addWidget(m_puerto, 1);
    filaConexion->addWidget(m_conectar, 2);
    connect(m_conectar, &QPushButton::clicked, this, &MainWindow::alPresionarConectar);
    m_estado = new QLabel(tr("Desconectado"));
    m_estado->setAlignment(Qt::AlignCenter);
    raiz->addWidget(cajaConexion);
    raiz->addWidget(m_estado);

    // 2) Cruceta de movimiento: botones grandes para el dedo
    auto *cajaMover = new QGroupBox(tr("Movimiento (mantén presionado)"));
    auto *cruceta = new QGridLayout(cajaMover);
    // Íconos estándar de Qt: se ven iguales en Windows, Linux y Android
    cruceta->addWidget(botonMovimiento(QStyle::SP_ArrowUp, "avanzar"), 0, 1);
    cruceta->addWidget(botonMovimiento(QStyle::SP_ArrowLeft, "izquierda"), 1, 0);
    cruceta->addWidget(botonMovimiento(QStyle::SP_MediaStop, "parar"), 1, 1);
    cruceta->addWidget(botonMovimiento(QStyle::SP_ArrowRight, "derecha"), 1, 2);
    cruceta->addWidget(botonMovimiento(QStyle::SP_ArrowDown, "retroceder"), 2, 1);
    raiz->addWidget(cajaMover);

    // 2b) Extra P3: grabar la secuencia de botones y repetirla en el robot
    auto *cajaRuta = new QGroupBox(tr("Ruta (grabar y repetir)"));
    auto *colRuta = new QVBoxLayout(cajaRuta);
    auto *filaRuta = new QHBoxLayout;
    m_grabar = new QPushButton(tr("Grabar"));
    m_repetirRuta = new QPushButton(tr("Repetir"));
    m_borrarRuta = new QPushButton(tr("Borrar"));
    for (auto *b : {m_grabar, m_repetirRuta, m_borrarRuta}) {
        b->setMinimumHeight(48);
        filaRuta->addWidget(b);
    }
    m_infoRuta = new QLabel;
    m_infoRuta->setAlignment(Qt::AlignCenter);
    colRuta->addLayout(filaRuta);
    colRuta->addWidget(m_infoRuta);
    connect(m_grabar, &QPushButton::clicked, this, &MainWindow::alPresionarGrabar);
    connect(m_repetirRuta, &QPushButton::clicked, this, &MainWindow::alPresionarRepetir);
    connect(m_borrarRuta, &QPushButton::clicked, this, [this] {
        m_pasos.clear();
        registrar(tr("ruta borrada"));
        actualizarRuta();
    });
    raiz->addWidget(cajaRuta);
    actualizarRuta();

    // 3) Servo
    auto *cajaServo = new QGroupBox(tr("Servo del sensor"));
    auto *filaServo = new QHBoxLayout(cajaServo);
    m_slider = new QSlider(Qt::Horizontal);
    m_slider->setRange(SERVO_MIN, SERVO_MAX);
    m_slider->setValue(90);
    m_angulo = new QLabel(QStringLiteral("90°"));
    m_angulo->setMinimumWidth(48);
    auto *barrido = new QPushButton(tr("Barrido"));
    filaServo->addWidget(m_slider, 1);
    filaServo->addWidget(m_angulo);
    filaServo->addWidget(barrido);
    connect(m_slider, &QSlider::valueChanged, this, &MainWindow::alMoverServo);
    connect(barrido, &QPushButton::clicked, this, &MainWindow::alPresionarBarrido);
    raiz->addWidget(cajaServo);

    // 4) Distancia: lectura y gráfica
    auto *cajaDist = new QGroupBox(tr("Distancia del HC-SR04"));
    auto *colDist = new QVBoxLayout(cajaDist);
    auto *filaDist = new QHBoxLayout;
    m_distancia = new QLabel(QStringLiteral("— cm"));
    QFont grande = m_distancia->font();
    // En Android la fuente viene en píxeles (pointSize() = -1): se parte del tamaño real
    grande.setPointSizeF(QFontInfo(grande).pointSizeF() + 6);
    grande.setBold(true);
    m_distancia->setFont(grande);
    auto *medir = new QPushButton(tr("Medir"));
    m_auto = new QCheckBox(tr("Automático"));
    filaDist->addWidget(m_distancia, 1);
    filaDist->addWidget(medir);
    filaDist->addWidget(m_auto);
    connect(medir, &QPushButton::clicked, this, &MainWindow::alPresionarMedir);
    connect(m_auto, &QCheckBox::toggled, this, [this](bool activo) {
        activo ? m_medirAuto.start() : m_medirAuto.stop();
    });
    colDist->addLayout(filaDist);

    m_serie = new QLineSeries;
    auto *grafica = new QChart;
    grafica->addSeries(m_serie);
    grafica->legend()->hide();
    m_ejeX = new QValueAxis;
    m_ejeX->setRange(0, VENTANA_MUESTRAS);
    m_ejeX->setLabelFormat("%d");
    m_ejeX->setTitleText(tr("muestra"));
    auto *ejeY = new QValueAxis;
    ejeY->setRange(0, 200);
    ejeY->setTitleText(tr("cm"));
    grafica->addAxis(m_ejeX, Qt::AlignBottom);
    grafica->addAxis(ejeY, Qt::AlignLeft);
    m_serie->attachAxis(m_ejeX);
    m_serie->attachAxis(ejeY);
    auto *vista = new QChartView(grafica);
    vista->setRenderHint(QPainter::Antialiasing);
    vista->setMinimumHeight(180);
    colDist->addWidget(vista, 1);
    raiz->addWidget(cajaDist, 1);

    // 5) Registro de mensajes
    m_registro = new QPlainTextEdit;
    m_registro->setReadOnly(true);
    m_registro->setMaximumBlockCount(200);
    m_registro->setMaximumHeight(110);
    raiz->addWidget(m_registro);

    // P4: lo de la P3 queda sin cambios en la pestaña «Robot»; lo nuevo, en «Temperatura»
    auto *pestanas = new QTabWidget(this);
    pestanas->addTab(central, tr("Robot"));
    auto *pagTemp = new QWidget;
    m_capaTemp = new QVBoxLayout(pagTemp);
    construirPestanaTemperatura(pagTemp);
    pestanas->addTab(pagTemp, tr("Temperatura"));
    setCentralWidget(pestanas);
    // P4: la pestaña Robot pide más alto que la pantalla del teléfono; sin esto, al
    // recargar la tabla la ventana crece a ese mínimo y el registro queda fuera de pantalla
    layout()->setSizeConstraint(QLayout::SetNoConstraint);
    setWindowTitle(tr("Robot P4 · Temperatura"));
}

// ---------------------------------------------------------------- P4: pestaña Temperatura
void MainWindow::construirPestanaTemperatura(QWidget *)
{
    m_capaTemp->setSpacing(8);

    // ALUMNO también aquí: debe verse en las capturas de esta pestaña
    auto *alumno = new QLabel(QString::fromUtf8(ALUMNO));
    alumno->setAlignment(Qt::AlignCenter);
    alumno->setWordWrap(true);
    alumno->setStyleSheet("font-weight: bold; color: #1F4E79;");
    m_capaTemp->addWidget(alumno);

    // E1: botón y última lectura
    auto *filaLeer = new QHBoxLayout;
    auto *leer = new QPushButton(tr("Leer temperatura"));
    leer->setMinimumHeight(48);
    m_temperatura = new QLabel(QStringLiteral("— °C"));
    QFont grande = m_temperatura->font();
    grande.setPointSizeF(QFontInfo(grande).pointSizeF() + 8);
    grande.setBold(true);
    m_temperatura->setFont(grande);
    m_temperatura->setAlignment(Qt::AlignCenter);
    filaLeer->addWidget(leer);
    filaLeer->addWidget(m_temperatura, 1);
    connect(leer, &QPushButton::clicked, this, [this] { enviar({{"tipo", "leer_dht"}}); });
    m_capaTemp->addLayout(filaLeer);

    // E3: guardado automático
    m_guardarAuto = new QCheckBox(tr("Guardar cada 10 s"));
    connect(m_guardarAuto, &QCheckBox::toggled, this, [this](bool activo) {
        activo ? m_leerTempAuto.start() : m_leerTempAuto.stop();
    });
    m_capaTemp->addWidget(m_guardarAuto);

    // E4: historial
    auto *filaHist = new QHBoxLayout;
    auto *cargar = new QPushButton(tr("Cargar historial"));
    cargar->setMinimumHeight(48);
    m_total = new QLabel(tr("Mediciones guardadas: ?"));
    filaHist->addWidget(cargar);
    filaHist->addWidget(m_total, 1);
    connect(cargar, &QPushButton::clicked, this, &MainWindow::cargarHistorial);
    m_capaTemp->addLayout(filaHist);

    m_tabla = new QTableWidget(0, 4);
    m_tabla->setHorizontalHeaderLabels({tr("id"), tr("fecha y hora"), tr("°C"), tr("nota")});
    m_tabla->setEditTriggers(QAbstractItemView::NoEditTriggers);    // solo lectura (ep. 03)
    m_tabla->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tabla->verticalHeader()->hide();   // así no se confunde el número de fila con el id
    m_tabla->horizontalHeader()->setStretchLastSection(true);
    m_capaTemp->addWidget(m_tabla, 1);

    // E5: nota con UPDATE ... WHERE id
    auto *cajaNota = new QGroupBox(tr("Anotar una medición (toca una fila)"));
    auto *filaNota = new QHBoxLayout(cajaNota);
    m_id = new QSpinBox;
    m_id->setRange(0, 99999);
    m_id->setSpecialValueText(QStringLiteral("—"));   // 0 = ninguna fila elegida
    m_id->setPrefix(tr("id "));
    m_nota = new QLineEdit;
    m_nota->setPlaceholderText(tr("Nota"));
    auto *guardarNota = new QPushButton(tr("Guardar nota"));
    filaNota->addWidget(m_id);
    filaNota->addWidget(m_nota, 1);
    filaNota->addWidget(guardarNota);
    connect(guardarNota, &QPushButton::clicked, this, &MainWindow::alPresionarGuardarNota);
    connect(m_tabla, &QTableWidget::cellClicked, this, [this](int fila, int) {
        m_id->setValue(m_tabla->item(fila, 0)->text().toInt());   // columna 0 = id
    });
    m_capaTemp->addWidget(cajaNota);
    m_resultadoNota = new QLabel;
    m_resultadoNota->setAlignment(Qt::AlignCenter);
    m_capaTemp->addWidget(m_resultadoNota);

    // Registro también en esta pestaña: la ruta de la base, el esquema y los
    // «Guardada … con id N» se ven junto con ALUMNO en la misma captura
    m_registroTemp = new QPlainTextEdit;
    m_registroTemp->setReadOnly(true);
    m_registroTemp->setMaximumBlockCount(200);
    m_registroTemp->setMinimumHeight(140);
    m_registroTemp->setMaximumHeight(150);   // el resto del alto es para la tabla (10+ filas)
    m_capaTemp->addWidget(m_registroTemp);
}

QPushButton *MainWindow::botonMovimiento(QStyle::StandardPixmap icono, const QString &accion)
{
    auto *b = new QPushButton(style()->standardIcon(icono), QString());
    b->setIconSize(QSize(40, 40));
    b->setMinimumSize(72, 64);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    b->setAccessibleName(accion);   // nombre para lectores de pantalla y pruebas automáticas
    // pressed/released funcionan igual con el ratón y con el dedo
    connect(b, &QPushButton::pressed, this, [this, accion] {
        // Extra P3: tocar la cruceta toma el control y cancela la repetición
        if (m_pasoActual >= 0)
            detenerRepeticion(tr("cancelada con la cruceta"));
        if (m_grabando && accion != "parar") {
            if (!m_pasos.isEmpty())   // la pausa antes del primer botón no se guarda
                anotarPaso("pausa", m_reloj.elapsed());
            m_accionGrabada = accion;
            m_reloj.restart();
        }
        if (accion == "parar") {
            m_repetir.stop();
            enviarMover("parar", 0);
            return;
        }
        m_accionActual = accion;
        enviarMover(accion, MS_MOVIMIENTO);
        m_repetir.start();
    });
    connect(b, &QPushButton::released, this, [this, accion] {
        if (accion == "parar") return;
        m_repetir.stop();
        enviarMover("parar", 0);
        if (m_grabando && m_accionGrabada == accion) {
            anotarPaso(accion, m_reloj.elapsed());
            m_accionGrabada.clear();
            m_reloj.restart();   // desde aquí corre la pausa hasta el siguiente botón
        }
    });
    return b;
}

// ---------------------------------------------------------------- conexión
void MainWindow::alPresionarConectar()
{
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.close();
        return;
    }
    const QUrl url(QStringLiteral("ws://%1:%2").arg(m_ip->text().trimmed()).arg(m_puerto->value()));
    m_estado->setText(tr("Conectando a %1 …").arg(url.toString()));
    m_conectar->setText(tr("Cancelar"));
    m_socket.open(url);
}

void MainWindow::alConectar()
{
    m_estado->setText(tr("Conectado a %1").arg(m_socket.requestUrl().toString()));
    m_conectar->setText(tr("Desconectar"));
    m_latido.start();
    enviar({{"tipo", "latido"}});
    registrar(tr("conectado"));
}

void MainWindow::alDesconectar()
{
    m_latido.stop();
    m_repetir.stop();
    m_medirAuto.stop();
    m_barrido.stop();
    m_leerTempAuto.stop();             // P4 · E3
    if (m_pasoActual >= 0)
        detenerRepeticion(tr("se perdió la conexión"));
    m_auto->setChecked(false);
    m_guardarAuto->setChecked(false);  // P4 · E3
    m_estado->setText(tr("Desconectado"));
    m_conectar->setText(tr("Conectar"));
    registrar(tr("desconectado"));
}

void MainWindow::alOcurrirError(QAbstractSocket::SocketError)
{
    registrar(tr("error: %1").arg(m_socket.errorString()));
}

void MainWindow::enviar(const QJsonObject &obj)
{
    if (m_socket.state() != QAbstractSocket::ConnectedState)
        return;
    m_socket.sendTextMessage(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
}

void MainWindow::enviarMover(const QString &accion, int ms)
{
    enviar({{"tipo", "mover"}, {"accion", accion}, {"ms", ms}});
}

void MainWindow::repetirMovimiento()
{
    enviarMover(m_accionActual, MS_MOVIMIENTO);
}

// ---------------------------------------------------------------- mensajes
void MainWindow::alRecibirMensaje(const QString &texto)
{
    const QJsonObject obj = QJsonDocument::fromJson(texto.toUtf8()).object();
    const QString tipo = obj.value("tipo").toString();

    if ((tipo == "distancia_lectura" || tipo == "scan") && obj.value("d").isNull()) {
        // lectura sin eco (ok:false): no se grafica como 0 cm
        registrar(tr("lectura inválida: %1").arg(obj.value("motivo").toString()));
    } else if (tipo == "distancia_lectura") {
        agregarDistancia(obj.value("d").toDouble());
    } else if (tipo == "scan") {
        agregarDistancia(obj.value("d").toDouble());
        registrar(tr("scan %1° → %2 cm").arg(obj.value("angulo").toInt())
                      .arg(obj.value("d").toDouble(), 0, 'f', 1));
    } else if (tipo == "dht_lectura") {
        // P4 · E1: se decide con dht_ok, no con el número (si el sensor falla puede
        // llegar null o la última lectura buena repetida)
        registrar("RX " + texto);
        const QJsonValue t = obj.value("temperatura");
        if (!obj.value("dht_ok").toBool() || !t.isDouble()) {
            registrar(tr("Lectura inválida: no se guarda"));
            m_temperatura->setText(tr("inválida"));
            return;
        }
        const double c1 = qRound(t.toDouble() * 10) / 10.0;   // el mismo valor que se guarda
        m_temperatura->setText(QString::number(c1, 'f', 1) + " °C");
        guardarTemperatura(c1);   // E3
    } else if (tipo == "saludo") {
        registrar(tr("saludo de %1").arg(obj.value("nombre_host").toString()));
    } else if (tipo == "mover_err") {
        registrar(tr("el robot rechazó la orden %1").arg(obj.value("accion").toString()));
    } else if (tipo == "latido_ok" || tipo == "telemetria" || tipo.startsWith("mover_")) {
        // frecuentes: no se registran para no llenar la pantalla
    } else {
        registrar(texto);
    }
}

// ---------------------------------------------------------------- servo y distancia
void MainWindow::alMoverServo(int angulo)
{
    m_angulo->setText(QStringLiteral("%1°").arg(angulo));
    m_anguloPendiente = angulo;
    if (!m_servo.isActive())
        m_servo.start();   // como máximo un mensaje cada 60 ms aunque el dedo se deslice rápido
}

void MainWindow::enviarServoPendiente()
{
    if (m_anguloPendiente < 0) return;
    enviar({{"tipo", "servo"}, {"angulo", m_anguloPendiente}});
    m_anguloPendiente = -1;
}

void MainWindow::alPresionarMedir()
{
    enviar({{"tipo", "leer_distancia"}});
}

void MainWindow::alPresionarBarrido()
{
    m_anguloBarrido = SERVO_MIN;
    m_barrido.start();
    siguientePasoBarrido();
}

void MainWindow::siguientePasoBarrido()
{
    if (m_anguloBarrido > SERVO_MAX) {
        m_barrido.stop();
        return;
    }
    enviar({{"tipo", "scan"}, {"angulo", m_anguloBarrido}});
    m_slider->blockSignals(true);   // mueve el deslizador sin mandar otra orden "servo"
    m_slider->setValue(m_anguloBarrido);
    m_slider->blockSignals(false);
    m_angulo->setText(QStringLiteral("%1°").arg(m_anguloBarrido));
    m_anguloBarrido += PASO_BARRIDO;
}

void MainWindow::agregarDistancia(double cm)
{
    m_distancia->setText(QStringLiteral("%1 cm").arg(cm, 0, 'f', 1));
    m_serie->append(m_muestra, cm);
    if (m_serie->count() > VENTANA_MUESTRAS)
        m_serie->remove(0);   // ventana deslizante: el buffer no crece sin límite
    ++m_muestra;
    m_ejeX->setRange(qMax(0, m_muestra - VENTANA_MUESTRAS), qMax(VENTANA_MUESTRAS, m_muestra));
}

// ---------------------------------------------------------------- extra P3: ruta
void MainWindow::alPresionarGrabar()
{
    if (m_pasoActual >= 0) return;
    m_grabando = !m_grabando;
    if (m_grabando) {
        m_pasos.clear();
        m_accionGrabada.clear();
        m_reloj.start();
        registrar(tr("grabando ruta: usa la cruceta"));
    } else {
        registrar(tr("ruta grabada: %1 pasos").arg(m_pasos.size()));
    }
    actualizarRuta();
}

void MainWindow::anotarPaso(const QString &accion, qint64 ms)
{
    if (ms < MIN_PASO_MS || m_pasos.size() >= MAX_PASOS)
        return;
    if (accion == "pausa")
        ms = qMin<qint64>(ms, MAX_PAUSA_MS);
    m_pasos.append({accion, int(ms)});
    actualizarRuta();
}

void MainWindow::alPresionarRepetir()
{
    if (m_pasoActual >= 0) {   // el mismo botón sirve para detener
        detenerRepeticion(tr("detenida por el usuario"));
        return;
    }
    if (m_grabando || m_pasos.isEmpty()) return;
    if (m_socket.state() != QAbstractSocket::ConnectedState) {
        registrar(tr("conecta el robot antes de repetir la ruta"));
        return;
    }
    registrar(tr("repitiendo ruta de %1 pasos").arg(m_pasos.size()));
    m_pasoActual = 0;
    siguientePasoRuta();
}

// Cada paso de movimiento reutiliza m_repetir, igual que si el dedo estuviera en el botón;
// al terminar su duración se manda "parar" y sigue el siguiente paso.
void MainWindow::siguientePasoRuta()
{
    m_repetir.stop();
    if (m_pasoActual > 0 && m_pasos.at(m_pasoActual - 1).accion != "pausa")
        enviarMover("parar", 0);
    if (m_pasoActual >= m_pasos.size()) {
        m_pasoActual = -1;
        registrar(tr("ruta terminada"));
        actualizarRuta();
        return;
    }
    const PasoRuta paso = m_pasos.at(m_pasoActual);
    registrar(tr("paso %1/%2: %3 %4 ms").arg(m_pasoActual + 1).arg(m_pasos.size())
                  .arg(paso.accion).arg(paso.ms));
    if (paso.accion != "pausa") {
        m_accionActual = paso.accion;
        enviarMover(paso.accion, qMin(MS_MOVIMIENTO, qMax(50, paso.ms)));
        m_repetir.start();
    }
    ++m_pasoActual;
    m_ruta.start(paso.ms);
    actualizarRuta();
}

void MainWindow::detenerRepeticion(const QString &motivo)
{
    m_ruta.stop();
    m_repetir.stop();
    m_pasoActual = -1;
    enviarMover("parar", 0);
    registrar(tr("ruta detenida: %1").arg(motivo));
    actualizarRuta();
}

void MainWindow::actualizarRuta()
{
    int totalMs = 0;
    for (const auto &p : m_pasos) totalMs += p.ms;
    const bool repitiendo = m_pasoActual >= 0;
    QString texto = tr("%1 pasos · %2 s").arg(m_pasos.size()).arg(totalMs / 1000.0, 0, 'f', 1);
    if (m_grabando) texto = tr("● Grabando · ") + texto;
    else if (repitiendo) texto = tr("▶ Paso %1 de %2").arg(m_pasoActual).arg(m_pasos.size());
    m_infoRuta->setText(texto);
    m_grabar->setText(m_grabando ? tr("Terminar") : tr("Grabar"));
    m_grabar->setEnabled(!repitiendo);
    m_repetirRuta->setText(repitiendo ? tr("Detener") : tr("Repetir"));
    m_repetirRuta->setEnabled(repitiendo || (!m_grabando && !m_pasos.isEmpty()));
    m_borrarRuta->setEnabled(!m_grabando && !repitiendo && !m_pasos.isEmpty());
}

void MainWindow::registrar(const QString &linea)
{
    const QString texto = QDateTime::currentDateTime().toString("hh:mm:ss  ") + linea;
    m_registro->appendPlainText(texto);
    if (m_registroTemp)
        m_registroTemp->appendPlainText(texto);   // P4: misma línea en la pestaña Temperatura
}

// ---------------------------------------------------------------- P4: base SQLite
// E2: abre (o crea) la base en la memoria privada de la app y crea la tabla
void MainWindow::abrirBase()
{
    const QString carpeta = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(carpeta);
    const QString ruta = carpeta + "/" + QString::fromUtf8(ARCHIVO_BASE);
    const bool yaExistia = QFile::exists(ruta);

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName(ruta);
    if (!db.open()) {
        registrar("No se abrió la base: " + db.lastError().text());
        return;
    }
    QSqlQuery consulta;
    if (!consulta.exec("CREATE TABLE IF NOT EXISTS mediciones ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT, "   // lo asigna SQLite
                       "fecha_hora TEXT NOT NULL, "    // cuándo llegó la lectura (ISO 8601)
                       "temperatura REAL NOT NULL, "   // °C con un decimal; nunca un 0 inventado
                       "fuente TEXT NOT NULL, "        // dht22 o simulada
                       "nota TEXT)")) {                // vacía; se llena con UPDATE en E5
        registrar("No se creó la tabla: " + consulta.lastError().text());
        return;
    }
    registrar((yaExistia ? "Base encontrada: " : "Base nueva creada: ") + ruta);

    // E2d: esquema real de la tabla (5 columnas) en el registro
    QSqlQuery esquema("PRAGMA table_info(mediciones)");
    while (esquema.next())
        registrar(QString("columna %1: %2 %3").arg(esquema.value(0).toInt())
                      .arg(esquema.value(1).toString(), esquema.value(2).toString()));

    cargarHistorial();
}

// E3: una fila nueva por cada lectura válida
void MainWindow::guardarTemperatura(double celsius)
{
    const double c1 = qRound(celsius * 10) / 10.0;   // un decimal (24.60000038 -> 24.6)
    QSqlQuery consulta;
    consulta.prepare("INSERT INTO mediciones (fecha_hora, temperatura, fuente) "
                     "VALUES (:fecha_hora, :temperatura, :fuente)");
    consulta.bindValue(":fecha_hora", QDateTime::currentDateTime().toString(Qt::ISODate));
    consulta.bindValue(":temperatura", c1);
    consulta.bindValue(":fuente", QString::fromUtf8(FUENTE));
    if (!consulta.exec()) {
        registrar("INSERT falló: " + consulta.lastError().text());
        return;
    }
    registrar(QString("Guardada %1 °C con id %2").arg(c1, 0, 'f', 1)
                  .arg(consulta.lastInsertId().toString()));
    cargarHistorial();   // E4: la tabla se recarga después de cada INSERT
}

// E4: las 50 más recientes arriba, y el total
void MainWindow::cargarHistorial()
{
    QSqlQuery consulta("SELECT id, fecha_hora, temperatura, nota FROM mediciones "
                       "ORDER BY id DESC LIMIT 50");
    if (!consulta.isActive()) {
        registrar("SELECT falló: " + consulta.lastError().text());
        return;
    }
    m_tabla->setRowCount(0);
    while (consulta.next()) {
        const int fila = m_tabla->rowCount();
        m_tabla->insertRow(fila);
        m_tabla->setItem(fila, 0, new QTableWidgetItem(consulta.value(0).toString()));
        m_tabla->setItem(fila, 1, new QTableWidgetItem(consulta.value(1).toString()));
        m_tabla->setItem(fila, 2, new QTableWidgetItem(
                              QString::number(consulta.value(2).toDouble(), 'f', 1)));
        m_tabla->setItem(fila, 3, new QTableWidgetItem(consulta.value(3).toString()));
    }
    m_tabla->resizeColumnsToContents();
    QSqlQuery total("SELECT COUNT(*) FROM mediciones");
    if (total.next())
        m_total->setText("Mediciones guardadas: " + total.value(0).toString());
}

// E5: escribe la nota en la fila con ese id (no crea filas nuevas)
void MainWindow::alPresionarGuardarNota()
{
    if (m_id->value() == 0) {
        m_resultadoNota->setText(tr("Primero toca una fila"));
        return;
    }
    QSqlQuery consulta;
    consulta.prepare("UPDATE mediciones SET nota = :nota WHERE id = :id");
    consulta.bindValue(":nota", m_nota->text().trimmed());
    consulta.bindValue(":id", m_id->value());
    if (!consulta.exec()) {
        m_resultadoNota->setText("UPDATE falló: " + consulta.lastError().text());
        return;
    }
    const int filas = consulta.numRowsAffected();
    m_resultadoNota->setText(QString("id %1 · Filas actualizadas: %2").arg(m_id->value()).arg(filas));
    registrar(QString("UPDATE id %1 -> %2 fila(s)").arg(m_id->value()).arg(filas));
    cargarHistorial();
}
