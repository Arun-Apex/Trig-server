FROM node:20-alpine
WORKDIR /app

COPY api/package.json api/package-lock.json* ./
RUN npm install --omit=dev

COPY api/server.js ./
COPY public ./public

EXPOSE 8082
CMD ["node", "server.js"]
