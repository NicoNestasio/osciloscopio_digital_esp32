import sys
import serial
import numpy as np
import pyqtgraph as pg
from pyqtgraph.Qt import QtWidgets, QtCore

# ==========================================
# CONFIGURACIÓN DEL SISTEMA
# ==========================================
PUERTO = 'COM11' # <-- REVISAR Y CAMBIAR AL COM DE TU ESP32
BAUDIOS = 921600
PUNTOS_PANTALLA = 400
V_REF = 3.3 

try:
    ser = serial.Serial(PUERTO, BAUDIOS, timeout=0.05)
    print(f"Conectado exitosamente a {PUERTO}")
except Exception as e:
    print(f"Error abriendo el puerto {PUERTO}: {e}")
    sys.exit(1)

# Variables globales para almacenar la configuración del instrumento
flanco_trigger = 0
nivel_trigger = 2048
tiempo_div_ms = 50
amplitud_div_v = 1.0

# ==========================================
# CONFIGURACIÓN DE LA INTERFAZ GRÁFICA
# ==========================================
app = QtWidgets.QApplication([])
win = QtWidgets.QWidget()
win.setWindowTitle("Osciloscopio Digital ESP32 - Grilla 10x10")
win.resize(1100, 700)
win.setStyleSheet("background-color: #121212;")

layout = QtWidgets.QVBoxLayout()
win.setLayout(layout)

# Panel Superior Dinámico
panel_stats = QtWidgets.QLabel("Esperando señal...")
panel_stats.setAlignment(QtCore.Qt.AlignCenter)
panel_stats.setStyleSheet("""
    font-family: monospace; 
    font-size: 13pt; 
    font-weight: bold;
    background-color: #1e1e1e; 
    color: white; 
    padding: 15px; 
    border-radius: 5px;
    border: 1px solid #333333;
""")
layout.addWidget(panel_stats)

# Lienzo del Gráfico
plot = pg.PlotWidget(title="Adquisición en Tiempo Real")
plot.showGrid(x=True, y=True, alpha=0.5)

plot.setLabel('left', 'Tensión', units='V')
plot.setLabel('bottom', 'Tiempo', units='ms')
layout.addWidget(plot)

curve = plot.plot(pen=pg.mkPen('c', width=2))

# ==========================================
# CURSORES MÓVILES
# ==========================================
cursor_v = pg.InfiniteLine(
    angle=0, movable=True, pos=1.65, 
    pen=pg.mkPen('y', style=QtCore.Qt.DashLine),
    label='Trig: {value:.2f} V', 
    labelOpts={'color': 'y', 'position': 0.05, 'fill': pg.mkBrush(0, 0, 0, 200)}
)
plot.addItem(cursor_v)

buffer_datos = []

# ==========================================
# ENVÍO DE COMANDOS POR TECLADO
# ==========================================
def manejar_teclado(event):
    tecla = event.text().lower()
    if tecla in ['t', 'a', 'f', '+', '-']:
        try:
            ser.write(tecla.encode('ascii'))
            print(f"Comando enviado al micro: {tecla}")
        except Exception as e:
            print(f"Error al enviar comando: {e}")

win.keyPressEvent = manejar_teclado

# ==========================================
# FUNCIÓN DE LECTURA Y ACTUALIZACIÓN
# ==========================================
def actualizar_grafico():
    global buffer_datos, flanco_trigger, nivel_trigger, tiempo_div_ms, amplitud_div_v
    
    try:
        lineas_leidas = 0
        while ser.in_waiting > 0 and lineas_leidas < 1000:
            linea = ser.readline().decode('ascii').strip()
            lineas_leidas += 1
            
            if linea.startswith("SYNC"):
                partes = linea.split(',')
                if len(partes) >= 5:
                    flanco_trigger = int(partes[1])
                    nivel_trigger = int(partes[2])
                    tiempo_div_ms = int(partes[3])
                    amplitud_div_v = float(partes[4])
                    
                    voltios_trigger = nivel_trigger * (V_REF / 4095.0)
                    cursor_v.setValue(voltios_trigger)
                    
                    # =======================================================
                    # MAGIA: EJE Y GRILLA FIJA 10x10 EN UNIDADES REALES
                    # =======================================================
                    
                    # 1. Eje X: Rango de 0 a (10 divisiones * ms/div)
                    rango_x = tiempo_div_ms * 10.0
                    plot.setXRange(0, rango_x, padding=0)
                    # Forzamos a PyQtGraph a poner una línea de grilla cada 'tiempo_div_ms'
                    plot.getAxis('bottom').setTickSpacing(levels=[(tiempo_div_ms, 0)])

                    # 2. Eje Y: Centramos en 1.65V, y damos 5 divisiones para arriba y 5 para abajo
                    centro_y = 1.65
                    rango_y_mitad = amplitud_div_v * 5.0
                    plot.setYRange(centro_y - rango_y_mitad, centro_y + rango_y_mitad, padding=0)
                    # Forzamos una línea de grilla cada 'amplitud_div_v'
                    plot.getAxis('left').setTickSpacing(levels=[(amplitud_div_v, 0)])
                    
            elif linea.isdigit():
                buffer_datos.append(int(linea))
                
    except (UnicodeDecodeError, ValueError):
        pass 
    except serial.SerialException:
        timer.stop()
        panel_stats.setText("<span style='color: #FF5555; font-size: 20pt;'>⚠️ CONEXIÓN PERDIDA (Cable desconectado) ⚠️</span>")
        return
        
    # Dibujado de la pantalla
    if len(buffer_datos) >= PUNTOS_PANTALLA:
        ventana_cruda = buffer_datos[-PUNTOS_PANTALLA:]
        buffer_datos.clear()
        
        # Eje X dinámico que cubre las 10 divisiones en milisegundos reales
        rango_x_total = tiempo_div_ms * 10.0
        x_tiempo_ms_dinamico = np.linspace(0, rango_x_total, PUNTOS_PANTALLA)
        
        # Eje Y en voltios reales
        y_volts = np.array(ventana_cruda) * (V_REF / 4095.0)
        
        curve.setData(x_tiempo_ms_dinamico, y_volts)
        
        v_max = np.max(y_volts)
        v_min = np.min(y_volts)
        v_pp = v_max - v_min
        
        txt_flanco = "SUBIDA" if flanco_trigger == 0 else "BAJADA"
        
        stats_html = f"""
            <span style='color: #FF5555; margin-right: 20px;'>Vmax: {v_max:.2f}V</span>
            <span style='color: #5555FF; margin-right: 20px;'>Vmin: {v_min:.2f}V</span>
            <span style='color: #55FF55; margin-right: 40px;'>Vpp: {v_pp:.2f}V</span>
            <span style='color: #FFFF55; margin-right: 20px;'>| T: {tiempo_div_ms}ms/div</span>
            <span style='color: #00FFFF; margin-right: 20px;'>A: {amplitud_div_v}V/div</span>
            <span style='color: #FF00FF;'>Flanco: {txt_flanco}</span>
        """
        panel_stats.setText(stats_html)

timer = QtCore.QTimer()
timer.timeout.connect(actualizar_grafico)
timer.start(10)

if __name__ == '__main__':
    print("Iniciando osciloscopio... (Cerrá la ventana para salir)")
    print("Controles activos: 't' (Tiempo), 'a' (Amplitud), 'f' (Flanco), '+' y '-' (Nivel Trigger)")
    win.show() 
    QtWidgets.QApplication.instance().exec_()
    ser.close()
    print("Puerto serie cerrado.")