#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QWebSocket>
#include <QTimer>
#include <QJsonObject>
#include <QStyle>
#include <QElapsedTimer>
#include <QVector>

class QLineEdit;
class QSpinBox;
class QPushButton;
class QLabel;
class QSlider;
class QCheckBox;
class QPlainTextEdit;
class QLineSeries;
class QValueAxis;
class QVBoxLayout;
class QTableWidget;
class QChart;
class QBarSeries;
class QBarCategoryAxis;

// Una sola ventana: conexión, cruceta de movimiento, servo y gráfica de distancia.
// La interfaz se arma en código (sin .ui) para que se adapte al tamaño del teléfono.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    // Conexión
    void alPresionarConectar();
    void alConectar();
    void alDesconectar();
    void alRecibirMensaje(const QString &texto);
    void alOcurrirError(QAbstractSocket::SocketError error);
    // Movimiento: mientras el botón esté presionado se repite la orden
    void repetirMovimiento();
    // Servo y distancia
    void alMoverServo(int angulo);
    void enviarServoPendiente();
    void alPresionarMedir();
    void alPresionarBarrido();
    void siguientePasoBarrido();
    // Extra P3: grabar y repetir una ruta
    void alPresionarGrabar();
    void alPresionarRepetir();
    void siguientePasoRuta();
    // Práctica 4: temperatura en SQLite
    void cargarHistorial();
    void alPresionarGuardarNota();
    void actualizarHistograma();      // E7 (extra)

private:
    void construirInterfaz();
    QPushButton *botonMovimiento(QStyle::StandardPixmap icono, const QString &accion);
    void enviar(const QJsonObject &obj);
    void enviarMover(const QString &accion, int ms);
    void agregarDistancia(double cm);
    void registrar(const QString &linea);
    void anotarPaso(const QString &accion, qint64 ms);
    void detenerRepeticion(const QString &motivo);
    void actualizarRuta();
    // Práctica 4
    void construirPestanaTemperatura(QWidget *pagina);
    void abrirBase();
    void guardarTemperatura(double celsius);
    void construirPestanaHistograma(QWidget *pagina);   // E7 (extra)

    QWebSocket m_socket;
    QTimer m_latido;          // {"tipo":"latido"} cada 1 s (la ESP32 responde latido_ok)
    QTimer m_repetir;         // reenvía "mover" mientras se mantiene presionado un botón
    QTimer m_servo;           // limita el envío del servo a uno cada 60 ms
    QTimer m_medirAuto;       // lectura periódica de distancia
    QTimer m_barrido;         // pasos del barrido con "scan"
    QTimer m_ruta;            // duración de cada paso al repetir la ruta
    QTimer m_leerTempAuto;    // P4: pide {"tipo":"leer_dht"} cada 10 s

    QString m_accionActual;   // acción repetida por m_repetir
    int m_anguloPendiente = -1;
    int m_anguloBarrido = 0;
    int m_muestra = 0;        // eje X de la gráfica (número de muestra)

    // Extra P3: la ruta es una lista de pasos {acción, duración}; "pausa" = robot detenido
    struct PasoRuta { QString accion; int ms; };
    QVector<PasoRuta> m_pasos;
    bool m_grabando = false;
    int m_pasoActual = -1;    // -1 = no se está repitiendo
    QElapsedTimer m_reloj;    // mide cuánto dura cada botón presionado y cada pausa
    QString m_accionGrabada;  // botón presionado durante la grabación

    // Widgets
    QLineEdit *m_ip = nullptr;
    QSpinBox *m_puerto = nullptr;
    QPushButton *m_conectar = nullptr;
    QLabel *m_estado = nullptr;
    QSlider *m_slider = nullptr;
    QLabel *m_angulo = nullptr;
    QLabel *m_distancia = nullptr;
    QCheckBox *m_auto = nullptr;
    QPlainTextEdit *m_registro = nullptr;
    QPushButton *m_grabar = nullptr;
    QPushButton *m_repetirRuta = nullptr;
    QPushButton *m_borrarRuta = nullptr;
    QLabel *m_infoRuta = nullptr;
    QLineSeries *m_serie = nullptr;
    QValueAxis *m_ejeX = nullptr;

    // Práctica 4: pestaña Temperatura
    QVBoxLayout *m_capaTemp = nullptr;
    QLabel *m_temperatura = nullptr;     // última lectura en °C
    QCheckBox *m_guardarAuto = nullptr;  // «Guardar cada 10 s»
    QTableWidget *m_tabla = nullptr;     // historial (SELECT)
    QLabel *m_total = nullptr;           // COUNT(*)
    QSpinBox *m_id = nullptr;            // id de la fila a anotar (E5)
    QLineEdit *m_nota = nullptr;
    QLabel *m_resultadoNota = nullptr;   // filas que cambió el UPDATE
    QPlainTextEdit *m_registroTemp = nullptr;  // copia del registro en esta pestaña

    // E7 (extra): histograma, cuántas lecturas cayeron en cada grado
    QBarSeries *m_serieHist = nullptr;
    QBarCategoryAxis *m_ejeGrados = nullptr;   // «22–23», «23–24»…
    QValueAxis *m_ejeConteo = nullptr;         // número de lecturas
    QLabel *m_resumenHist = nullptr;
};

#endif // MAINWINDOW_H
