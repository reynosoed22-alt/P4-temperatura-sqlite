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

// Regla antiplagio de la Práctica 3: escribe aquí tu nombre y tu código.
// Debe verse en todas tus capturas del teléfono.
static const char *ALUMNO = "Edgar Ulises Reynoso Gonzalez · 223823098";

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
}

// ---------------------------------------------------------------- interfaz
void MainWindow::construirInterfaz()
{
    auto *central = new QWidget(this);
    auto *raiz = new QVBoxLayout(central);
    raiz->setSpacing(8);

    auto *alumno = new QLabel(QString::fromUtf8(ALUMNO));
    alumno->setAlignment(Qt::AlignCenter);
    alumno->setStyleSheet("font-weight: bold; color: #1F4E79;");
    raiz->addWidget(alumno);

    // 1) Conexión
    auto *cajaConexion = new QGroupBox(tr("Conexión con el robot"));
    auto *filaConexion = new QHBoxLayout(cajaConexion);
    m_ip = new QLineEdit(QStringLiteral("192.168.1.101"));
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

    setCentralWidget(central);
    setWindowTitle(tr("Robot P3 · Android"));
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
    if (m_pasoActual >= 0)
        detenerRepeticion(tr("se perdió la conexión"));
    m_auto->setChecked(false);
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
    m_registro->appendPlainText(QDateTime::currentDateTime().toString("hh:mm:ss  ") + linea);
}
