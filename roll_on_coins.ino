#include "TFT_eSPI.h"
#include "LIS3DHTR.h"

#define SCREEN_W   320
#define SCREEN_H   240
#define BANNER_H    40
#define GAME_X0     0
#define GAME_Y0     BANNER_H
#define GAME_W      SCREEN_W
#define GAME_H      (SCREEN_H - BANNER_H)
#define BALL_R       7
#define COIN_R       5
#define NUM_COINS    5
#define TIMER_INIT   30  // secondes
#define POINTS_COIN  10  // points par pièce jaune
#define POINTS_EXTRA  5  // points par pièce jaune après usage joker
#define PENALTY_TIME 10  // secondes perdues sur pièce noire (expert)
#define JOKER_TIME   10  // secondes ajoutées par le joker
#define COL_BG       TFT_DARKGREY
#define COL_BANNER   TFT_BLUE
#define COL_TEXT     TFT_WHITE
#define COL_BALL     TFT_RED
#define COL_COIN     TFT_YELLOW
#define COL_BAD      TFT_BLACK
#define MARGIN 2
#define ACCEL_SCALE  2.5  // sensibilité de la gravité (ajustable)
#define FRICTION     0.92 // facteur de décélération (0‑1)
#define LOOP_MS      30    // période de boucle en ms (~33 fps)

enum Level { NOVICE = 0, CONFIRME, EXPERT };
const char* levelNames[] = { "Novice", "Confirme", "Expert" };

#define BTN_LEVEL   WIO_KEY_C   // bouton gauche  → changer niveau
#define BTN_START   WIO_KEY_B   // bouton milieu  → lancer partie
#define BTN_JOKER   WIO_KEY_A   // bouton droite  → +10 s (1x/partie)

struct Coin {
  int   x, y;
  bool  active;
  bool  bad;     // true = pièce noire (expert seulement)
};

// ── Variables globales ────────────────────────────────────────
TFT_eSPI tft;
LIS3DHTR<TwoWire> lis;

Level   currentLevel  = NOVICE;
int     score         = 0;
int     timeLeft      = TIMER_INIT;
bool    gameRunning   = false;
bool    jokerUsed     = false;
int     coinValue     = POINTS_COIN;  // peut passer à POINTS_EXTRA

float   bx, by;    // position boule (float pour la physique)
float   vx = 0, vy = 0;  // vitesse

Coin    coins[NUM_COINS + 3];  // +3 pièces noires max (expert)
int     totalCoins   = 0;
int     totalBad     = 0;

unsigned long lastLoopMs  = 0;
unsigned long lastSecondMs = 0;

// ── Maps des niveaux ──────────────────────────────────────────
// Coordonnées relatives à la zone de jeu (0,0) = coin haut-gauche zone
// Format : { x, y, bad }
struct CoinDef { int x, y; bool bad; };

const CoinDef mapNovice[NUM_COINS] = {
  {160, 60,  false},
  { 80, 100, false},
  {240, 100, false},
  {120, 150, false},
  {200, 150, false},
};

const CoinDef mapConfirme[NUM_COINS] = {
  {  COIN_R + MARGIN,   COIN_R + MARGIN, false},  // coin haut-gauche
  {GAME_W - COIN_R - MARGIN, COIN_R + MARGIN, false}, // coin haut-droite
  {160,           GAME_H / 2, false},
  {  COIN_R + MARGIN, GAME_H - COIN_R - MARGIN, false}, // coin bas-gauche
  {GAME_W - COIN_R - MARGIN, GAME_H - COIN_R - MARGIN, false}, // coin bas-droite
};

// Expert = mêmes pièces jaunes que confirmé + 3 pièces noires
const CoinDef mapExpertBad[3] = {
  {160,  40, true},
  { 60, GAME_H - 40, true},
  {260, GAME_H - 40, true},
};

// ── Prototypes ────────────────────────────────────────────────
void drawBanner();
void drawGame();
void drawBall(int x, int y, unsigned int color);
void drawCoin(const Coin& c);
void loadMap(Level lv);
void startGame();
void endGame(bool success, bool borderHit);
void updatePhysics();
void checkCollisions();
void handleButtons();
void buzzHappy();
void buzzSad();
void buzzPickup();
void buzzBad();
void buzzStart();
void buzzTimeUp();
void tone_(int freq, int dur);

// ─────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // Écran
  tft.begin();
  tft.setRotation(3); // (0,0) opposé au joystick
  tft.fillScreen(COL_BG);

  // Accéléromètre
  lis.begin(Wire1);
  if (!lis) { Serial.println("Accel ERROR"); while (1); }
  lis.setOutputDataRate(LIS3DHTR_DATARATE_25HZ);
  lis.setFullScaleRange(LIS3DHTR_RANGE_2G);

  // Buzzer
  pinMode(WIO_BUZZER, OUTPUT);

  // Boutons
  pinMode(BTN_LEVEL, INPUT_PULLUP);
  pinMode(BTN_START, INPUT_PULLUP);
  pinMode(BTN_JOKER, INPUT_PULLUP);

  // Affichage initial
  loadMap(currentLevel);
  drawBanner();
  drawGame();
}

// ─────────────────────────────────────────────────────────────
// LOOP
// ─────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  if (!gameRunning) {
    handleButtons();
    return;
  }

  // Décompte du chrono (1 seconde)
  if (now - lastSecondMs >= 1000) {
    lastSecondMs = now;
    timeLeft--;
    drawBanner();

    if (timeLeft <= 0) {
      timeLeft = 0;
      drawBanner();
      buzzTimeUp();
      endGame(false, false);
      return;
    }
  }

  // Joker : +10 s une seule fois par partie (bouton droit)
  if (!jokerUsed && digitalRead(BTN_JOKER) == LOW) {
    delay(200); // anti-rebond
    jokerUsed = true;
    coinValue  = POINTS_EXTRA;
    timeLeft  += JOKER_TIME;
    drawBanner();
    tone_(600, 150);
  }

  // Physique à la cadence LOOP_MS
  if (now - lastLoopMs >= LOOP_MS) {
    lastLoopMs = now;
    updatePhysics();
    checkCollisions();
  }
}

// ─────────────────────────────────────────────────────────────
// PHYSIQUE
// ─────────────────────────────────────────────────────────────
void updatePhysics() {
  float ax = lis.getAccelerationX();
  float ay = lis.getAccelerationY();

  // L'axe Y de l'accéléromètre → mouvement horizontal
  // L'axe X de l'accéléromètre → mouvement vertical
  vx += -ay * ACCEL_SCALE;
  vy +=  ax * ACCEL_SCALE;

  vx *= FRICTION;
  vy *= FRICTION;

  // Effacer ancienne boule
  drawBall((int)bx, (int)by, COL_BG);

  bx += vx;
  by += vy;

  // ── Collision avec les bords (zone de jeu absolue) ────────
  int absX0 = GAME_X0 + BALL_R;
  int absX1 = GAME_X0 + GAME_W - BALL_R;
  int absY0 = GAME_Y0 + BALL_R;
  int absY1 = GAME_Y0 + GAME_H - BALL_R;

  if (bx < absX0 || bx > absX1 || by < absY0 || by > absY1) {
    // Sortie de zone → échec
    bx = constrain(bx, absX0, absX1);
    by = constrain(by, absY0, absY1);
    drawBall((int)bx, (int)by, COL_BALL);
    buzzSad();
    endGame(false, true);
    return;
  }

  drawBall((int)bx, (int)by, COL_BALL);
}

// ─────────────────────────────────────────────────────────────
// COLLISIONS AVEC LES PIÈCES
// ─────────────────────────────────────────────────────────────
void checkCollisions() {
  int coinsLeft = 0;

  for (int i = 0; i < totalCoins + totalBad; i++) {
    if (!coins[i].active) continue;
    if (!coins[i].bad) coinsLeft++;

    // Distance centre-à-centre
    float dx = bx - (GAME_X0 + coins[i].x);
    float dy = by - (GAME_Y0 + coins[i].y);
    float dist = sqrt(dx * dx + dy * dy);

    if (dist < (BALL_R + COIN_R)) {
      coins[i].active = false;

      // Effacer la pièce (redessiner fond)
      tft.fillCircle(GAME_X0 + coins[i].x, GAME_Y0 + coins[i].y,
                     COIN_R + 1, COL_BG);

      if (coins[i].bad) {
        // Pièce noire
        timeLeft -= PENALTY_TIME;
        if (timeLeft < 0) timeLeft = 0;
        buzzBad();
        drawBanner();
      } else {
        // Pièce jaune
        score += coinValue;
        buzzPickup();
        drawBanner();
        coinsLeft--; // on vient de la compter active, on corrige
      }
    }
  }

  // Vérifier si toutes les pièces jaunes sont ramassées
  int yellowLeft = 0;
  for (int i = 0; i < totalCoins + totalBad; i++) {
    if (coins[i].active && !coins[i].bad) yellowLeft++;
  }
  if (yellowLeft == 0) {
    // Victoire
    if (!jokerUsed) score += timeLeft; // bonus temps uniquement sans joker
    buzzHappy();
    endGame(true, false);
  }
}

// ─────────────────────────────────────────────────────────────
// GESTION DES BOUTONS (hors partie)
// ─────────────────────────────────────────────────────────────
void handleButtons() {
  // Changer de niveau
  if (digitalRead(BTN_LEVEL) == LOW) {
    delay(200); // anti-rebond simple
    currentLevel = (Level)((currentLevel + 1) % 3);
    loadMap(currentLevel);
    drawBanner();
    drawGame();
    delay(200);
  }

  // Lancer une partie
  if (digitalRead(BTN_START) == LOW) {
    delay(200);
    startGame();
  }

  // Le bouton joker n'est actif que pendant la partie
}

// ─────────────────────────────────────────────────────────────
// DÉMARRAGE D'UNE PARTIE
// ─────────────────────────────────────────────────────────────
void startGame() {
  score      = 0;
  timeLeft   = TIMER_INIT;
  jokerUsed  = false;
  coinValue  = POINTS_COIN;
  vx = 0; vy = 0;

  // Boule au centre de la zone de jeu
  bx = GAME_X0 + GAME_W / 2.0;
  by = GAME_Y0 + GAME_H / 2.0;

  loadMap(currentLevel);
  drawBanner();
  drawGame();

  // Compte à rebours visuel + bip de départ
  for (int i = 3; i >= 1; i--) {
    tft.setTextColor(TFT_WHITE, COL_BG);
    tft.setTextSize(4);
    tft.drawNumber(i, bx - 10, by - 16);
    delay(800);
    tft.fillRect(bx - 20, by - 24, 60, 48, COL_BG); // effacer chiffre
  }
  buzzStart();

  gameRunning    = true;
  lastLoopMs     = millis();
  lastSecondMs   = millis();
}

// ─────────────────────────────────────────────────────────────
// FIN DE PARTIE
// ─────────────────────────────────────────────────────────────
void endGame(bool success, bool borderHit) {
  gameRunning = false;

  if (borderHit) score = 0;

  drawBanner();

  // Message résultat
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, COL_BG);
  if (success) {
    tft.drawString("Bravo !", GAME_X0 + GAME_W / 2 - 40, GAME_Y0 + GAME_H / 2 - 10);
  } else if (borderHit) {
    tft.drawString("Sorti !", GAME_X0 + GAME_W / 2 - 40, GAME_Y0 + GAME_H / 2 - 10);
  } else {
    tft.drawString("Temps fini!", GAME_X0 + GAME_W / 2 - 40, GAME_Y0 + GAME_H / 2 - 10);
  }

  delay(3000);

  // Retour à l'écran d'accueil
  tft.fillRect(GAME_X0, GAME_Y0, GAME_W, GAME_H, COL_BG);
  loadMap(currentLevel);
  drawGame();
}

// ─────────────────────────────────────────────────────────────
// CHARGEMENT DE LA MAP
// ─────────────────────────────────────────────────────────────
void loadMap(Level lv) {
  totalBad = 0;

  const CoinDef* yellow = (lv == NOVICE) ? mapNovice : mapConfirme;

  for (int i = 0; i < NUM_COINS; i++) {
    coins[i] = { yellow[i].x, yellow[i].y, true, false };
  }
  totalCoins = NUM_COINS;

  if (lv == EXPERT) {
    for (int i = 0; i < 3; i++) {
      coins[NUM_COINS + i] = { mapExpertBad[i].x, mapExpertBad[i].y, true, true };
    }
    totalBad = 3;
  }
}

// ─────────────────────────────────────────────────────────────
// DESSIN DU BANDEAU
// ─────────────────────────────────────────────────────────────
void drawBanner() {
  tft.fillRect(0, 0, SCREEN_W, BANNER_H, COL_BANNER);
  tft.setTextColor(COL_TEXT);
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);

  int y = BANNER_H / 2;

  // --- Niveau (gauche, mais pas collé)
  tft.setTextDatum(ML_DATUM);
  tft.drawString(levelNames[currentLevel], 6, y);

  // --- Score (centre parfait écran)
  String scoreStr = String(score) + " pts";
  tft.setTextDatum(MC_DATUM);
  tft.drawString(scoreStr, SCREEN_W / 2, y);

  // --- Timer (droite, mais avec marge)
  String timeStr = String(timeLeft) + "s";
  tft.setTextDatum(MR_DATUM);
  tft.drawString(timeStr, SCREEN_W - 6, y);
}

// ─────────────────────────────────────────────────────────────
// DESSIN DE LA ZONE DE JEU (fond + pièces)
// ─────────────────────────────────────────────────────────────
void drawGame() {
  tft.fillRect(GAME_X0, GAME_Y0, GAME_W, GAME_H, COL_BG);

  // Dessiner les pièces
  for (int i = 0; i < totalCoins + totalBad; i++) {
    if (coins[i].active) drawCoin(coins[i]);
  }

  // Dessiner la boule si elle est initialisée (hors partie → centre)
  if (!gameRunning) {
    int cx = GAME_X0 + GAME_W / 2;
    int cy = GAME_Y0 + GAME_H / 2;
    tft.fillCircle(cx, cy, BALL_R, COL_BALL);
  } else {
    drawBall((int)bx, (int)by, COL_BALL);
  }
}

// ─────────────────────────────────────────────────────────────
// HELPERS DESSIN
// ─────────────────────────────────────────────────────────────
void drawBall(int x, int y, unsigned int color) {
  tft.fillCircle(x, y, BALL_R, color);
}
void drawCoin(const Coin& c) {
  unsigned int col = c.bad ? COL_BAD : COL_COIN;
  int ax = GAME_X0 + c.x;
  int ay = GAME_Y0 + c.y;

  // Pièce ronde
  tft.fillCircle(ax, ay, COIN_R, col);

  if (!c.bad) {
    // Contour
    tft.drawCircle(ax, ay, COIN_R, TFT_ORANGE);

    // Petit symbole dollar bien adapté
    tft.setTextSize(1);
    tft.setTextColor(TFT_BLACK, col);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("$", ax, ay);
  }
}
// ─────────────────────────────────────────────────────────────
// SONS (buzzer passif WIO_BUZZER)
// ─────────────────────────────────────────────────────────────
void tone_(int freq, int dur) {
  analogWrite(WIO_BUZZER, 128);
  // Fréquence simulée par délai : on utilise les fonctions Arduino tone()
  // si disponibles, sinon on génère manuellement
  tone(WIO_BUZZER, freq, dur);
  delay(dur + 20);
  noTone(WIO_BUZZER);
}

void buzzPickup() {
  tone_(1200, 80);
  tone_(1600, 80);
}

void buzzBad() {
  tone_(300, 200);
  tone_(200, 200);
}

void buzzStart() {
  tone_(440, 100);
  tone_(880, 400);
}

void buzzHappy() {
  // Petite mélodie joyeuse
  int notes[] = {523, 659, 784, 1047};
  for (int n : notes) { tone_(n, 120); delay(20); }
}

void buzzSad() {
  tone_(400, 150);
  tone_(300, 150);
  tone_(200, 300);
}

void buzzTimeUp() {
  for (int i = 0; i < 5; i++) {
    tone_(800, 100);
    delay(150);
  }
}
