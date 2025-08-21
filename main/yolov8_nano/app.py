from flask import Flask, request, jsonify, send_from_directory, render_template
import os
import zipfile
import shutil
import random
import subprocess
from werkzeug.utils import secure_filename

app = Flask(__name__, template_folder='templates')
app.config['UPLOAD_FOLDER'] = 'uploads'
app.config['DATASET_FOLDER'] = 'datasets'
# 确保目录存在
os.makedirs(app.config['UPLOAD_FOLDER'], exist_ok=True)
os.makedirs(app.config['DATASET_FOLDER'], exist_ok=True)
# os.makedirs('templates', exist_ok=True)

@app.route('/')
def index():
    """返回训练系统界面"""
    return render_template('index.html')

# 文件上传处理
@app.route('/upload', methods=['POST'])
def upload_file():
    if 'file' not in request.files:
        return jsonify({'error': 'No file uploaded'}), 400
    file = request.files['file']
    if file.filename == '':
        return jsonify({'error': 'Empty filename'}), 400
    filename = secure_filename(file.filename)
    file.save(os.path.join(app.config['UPLOAD_FOLDER'], filename))
    return jsonify({'filename': filename})

# 自动分割数据集
@app.route('/split', methods=['POST'])
def split_dataset():
    # 保存上传的ZIP文件
    file = request.files['dataset']
    zip_path = os.path.join(app.config['UPLOAD_FOLDER'], secure_filename(file.filename))
    file.save(zip_path)
    
    # 解压到datasets目录
    with zipfile.ZipFile(zip_path, 'r') as zip_ref:
        zip_ref.extractall(app.config['DATASET_FOLDER'])

    # 分割数据集（8:2比例）
    images = [f for f in os.listdir(f"{app.config['DATASET_FOLDER']}/raw_images") if f.endswith(('.jpg', '.png'))]
    random.shuffle(images)
    split_idx = int(len(images) * 0.8)
    
    # 创建目录结构
    os.makedirs(f"{app.config['DATASET_FOLDER']}/images/train", exist_ok=True)
    os.makedirs(f"{app.config['DATASET_FOLDER']}/images/val", exist_ok=True)
    os.makedirs(f"{app.config['DATASET_FOLDER']}/labels/train", exist_ok=True)
    os.makedirs(f"{app.config['DATASET_FOLDER']}/labels/val", exist_ok=True)
    
    # 复制文件到对应目录
    for i, img in enumerate(images):
        label = os.path.splitext(img)[0] + '.txt'
        if i < split_idx:
            shutil.copy(f"{app.config['DATASET_FOLDER']}/raw_images/{img}", f"{app.config['DATASET_FOLDER']}/images/train/{img}")
            shutil.copy(f"{app.config['DATASET_FOLDER']}/raw_labels/{label}", f"{app.config['DATASET_FOLDER']}/labels/train/{label}")
        else:
            shutil.copy(f"{app.config['DATASET_FOLDER']}/raw_images/{img}", f"{app.config['DATASET_FOLDER']}/images/val/{img}")
            shutil.copy(f"{app.config['DATASET_FOLDER']}/raw_labels/{label}", f"{app.config['DATASET_FOLDER']}/labels/val/{label}")

    return jsonify({
        'train_count': split_idx,
        'val_count': len(images) - split_idx
    })

# 训练接口
@app.route('/train', methods=['POST'])
def train():
    data = request.json
    classes = data['classes']
    epochs = data['epochs']
    batch = data['batch']
    imgsz = data['imgsz']
    
    # 生成YAML配置文件
    with open(f"{app.config['DATASET_FOLDER']}/data.yaml", 'w') as f:
        f.write(f"path: {app.config['DATASET_FOLDER']}\n")
        f.write(f"train: images/train\n")
        f.write(f"val: images/val\n")
        f.write(f"nc: {len(classes)}\n")
        f.write(f"names: {classes}\n")
    
    # 启动训练
    cmd = f"python train_esp32.py --data {app.config['DATASET_FOLDER']}/data.yaml --epochs {epochs} --batch {batch} --imgsz {imgsz}"
    process = subprocess.Popen(cmd, 
        shell=True, 
        stdout=subprocess.PIPE, 
        stderr=subprocess.STDOUT, 
        text=True,
        encoding='utf-8', # 显式指定编码
        errors='replace'   # 替换非法字符
        )
    
    # 实时返回训练日志
    def generate():
        for line in process.stdout:
            yield line

    return app.response_class(generate(), mimetype='text/plain')

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, debug=True)