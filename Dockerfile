FROM nodered/node-red:latest

# Install required nodes
RUN npm install \
    node-red-dashboard \
    node-red-contrib-influxdb \
    node-red-node-email
