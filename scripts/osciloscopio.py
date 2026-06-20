import sys
import serial
import numpy as np
import pyqtgraph as pg
from pyqtgraph.Qt import QtWidgets, QtCore

# ==========================================
# CONFIGURACIÓN DEL SISTEMA
# ==========================================
PUERTO = 'COM11'
BAUDIOS = 921600
PUNTOS_PANTALLA = 400
V_REF = 3.3 
FS = 20000.0 # Frecuencia de muestreo en Hz (20 kHz)

try:
    ser = serial.Serial(PUERTO, BAUDIOS, timeout=0.05)
    print(f"Conectado exitosamente a {PUERTO}")
except Exception as e:
    print(f"Error abriendo el puerto {PUERTO}: {e}")
    sys.exit(1)

# ==========================================
# CÁLCULO DEL EJE X (TIEMPO)
# ==========================================
# Creamos un arreglo que va de 0 a 19.95 ms, con pasos de 0.05 ms (50 us)
x_tiempo_ms = np.arange(PUNTOS_PANTALLA) * (1000.0 / FS)

# ==========================================
# CONFIGURACIÓN DE LA INTERFAZ GRÁFICA
# ==========================================
app = QtWidgets.QApplication([])
win = QtWidgets.QWidget()
win.setWindowTitle("Osciloscopio Digital ESP32")
win.resize(1000, 650)
win.setStyleSheet("background-color: #121212;")

# Creamos un Layout Vertical para apilar el texto arriba y el gráfico abajo
layout = QtWidgets.QVBoxLayout()
win.setLayout(layout)

# 1. Panel Superior (Display de Estadísticas)
panel_stats = QtWidgets.QLabel("Esperando señal...")
panel_stats.setAlignment(QtCore.Qt.AlignCenter)
panel_stats.setStyleSheet("""
    font-family: monospace; 
    font-size: 16pt; 
    font-weight: bold;
    background-color: #1e1e1e; 
    color: white; 
    padding: 15px; 
    border-radius: 5px;
    border: 1px solid #333333;
""")
layout.addWidget(panel_stats)

# 2. Lienzo del Gráfico (Abajo)
plot = pg.PlotWidget(title="Adquisición en Tiempo Real")
plot.setYRange(0, 3.5)
plot.setXRange(0, x_tiempo_ms[-1]) # Bloqueamos el eje X entre 0 y 20 ms
plot.showGrid(x=True, y=True, alpha=0.5)

plot.setLabel('left', 'Tensión', units='V')
plot.setLabel('bottom', 'Tiempo', units='ms')
layout.addWidget(plot)

curve = plot.plot(pen=pg.mkPen('c', width=2))

# ==========================================
# CURSORES MÓVILES
# ==========================================
# Cursor de Tensión (Horizontal)
cursor_v = pg.InfiniteLine(
    angle=0, movable=True, pos=1.65, 
    pen=pg.mkPen('y', style=QtCore.Qt.DashLine),
    label='{value:.2f} V', 
    labelOpts={'color': 'y', 'position': 0.05, 'fill': pg.mkBrush(0, 0, 0, 200)}
)
plot.addItem(cursor_v)

# Cursor de Tiempo (Vertical) - Inicia en el centro (10 ms)
cursor_t = pg.InfiniteLine(
    angle=90, movable=True, pos=10.0, 
    pen=pg.mkPen('m', style=QtCore.Qt.DashLine),
    label='{value:.2f} ms', 
    labelOpts={'color': 'm', 'position': 0.95, 'fill': pg.mkBrush(0, 0, 0, 200)}
)
plot.addItem(cursor_t)

buffer_datos = []

# ==========================================
# FUNCIÓN DE LECTURA Y ACTUALIZACIÓN
# ==========================================
def actualizar_grafico():
    global buffer_datos
    
    while ser.in_waiting > 0:
        try:
            linea = ser.readline().decode('ascii').strip()
            if linea.isdigit():
                buffer_datos.append(int(linea))
        except UnicodeDecodeError:
            pass 
            
    if len(buffer_datos) >= PUNTOS_PANTALLA:
        ventana_cruda = buffer_datos[-PUNTOS_PANTALLA:]
        buffer_datos.clear()
        
        # Conversión a tensión
        y_volts = np.array(ventana_cruda) * (V_REF / 4095.0)
        
        # Le pasamos tanto el eje X (tiempo) como el eje Y (tensión)
        curve.setData(x_tiempo_ms, y_volts)
        
        # Cálculos de picos
        v_max = np.max(y_volts)
        v_min = np.min(y_volts)
        v_pp = v_max - v_min
        
        # Actualizamos el panel superior (con colores HTML)
        stats_html = f"""
            <span style='color: #FF5555; margin-right: 30px;'>Vmax: {v_max:.2f} V</span>
            <span style='color: #5555FF; margin-right: 30px;'>Vmin: {v_min:.2f} V</span>
            <span style='color: #55FF55;'>Vpp: {v_pp:.2f} V</span>
        """
        panel_stats.setText(stats_html)

timer = QtCore.QTimer()
timer.timeout.connect(actualizar_grafico)
timer.start(10)

if __name__ == '__main__':
    print("Iniciando osciloscopio... (Cerrá la ventana para salir)")
    win.show() # Usamos win.show() en lugar de arrancar el GraphicsLayoutWidget
    QtWidgets.QApplication.instance().exec_()
    ser.close()
    print("Puerto serie cerrado.")