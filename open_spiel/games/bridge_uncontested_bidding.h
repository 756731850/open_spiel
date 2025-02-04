// Copyright 2019 DeepMind Technologies Ltd. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef THIRD_PARTY_OPEN_SPIEL_GAMES_BRIDGE_UNCONTESTED_BIDDING_H_
#define THIRD_PARTY_OPEN_SPIEL_GAMES_BRIDGE_UNCONTESTED_BIDDING_H_

#include <array>

// Uncontested bridge bidding. A two-player purely cooperative game.
//
// The full game of contract bridge is played by four players in two
// partnerships; it consists of a bidding phase followed by a play phase. The
// bidding phase determines the contract for the play phase. The contract has
// four components:
//    - Which of the four players is the 'declarer'. (The first play is made by
//      the player on declarer's left. Declarer's partner (the 'dummy') then
//      places their cards face-up for everyone to see; their plays are chosen
//      by declarer.)
//    - The trump suit (or no-trumps).
//    - The level, i.e. the trick target for the declaring partnership.
//    - Whether the contract is doubled or redoubled (increasing the stakes).
//
// In 'uncontested bidding', we simplify the game in two ways:
//   1. Only one partnership may bid during the auction phase (hence
//      'uncontested').
//   2. Rather than play out the play phase, we generate several (e.g. 10)
//      layouts of the opponents' cards, solve for the number of tricks that
//      would be taken with perfect perfect-information play by both sides
//      on each deal, and use the average score over these deals. (This
//      perfect information solution is called 'double dummy', because it is
//      equivalent to one player of each partnerships being 'dummy' in the sense
//      described above).
//
// Since the other partnership has no actions available, this is
// a two-player cooperative game. It is widely used by partnerships
// to practice their bidding. See for example this on-line tool:
// http://www.bridgebase.com/help/v2help/partnership_bidding.html
// Or these pre-constructed hands:
// http://rpbridge.net/rpbp.htm (here the scores are generated using human
// judgement rather than the automated procedure given above).
//
// We support two variations:
//   1. Any deal permitted, auction starts normally.
//      In this variant, WBridge5 scores +95.1 absolute, std err 2.2
//      Its relative score (compared to the best-possible score on each deal)
//      is -68.8, std err 1.3 (both results from n=8750 deals).
//   2. First player is dealt a hand suitable for a 2NT opening (i.e. a bid
//      of 8 tricks with no trumps), and is forced to bid 2NT.
//      A 2NT opening is typically played as showing a very strong balanced
//      hand. 'Balanced' means that the distribution of cards between the
//      suits is 4-3-3-3, 4-4-3-2, or 5-3-3-2 (some might also incude some
//      6-3-2-2 or 5-4-2-2 hands, but we do not).
//      Strength is typically measured using 'high card points' (A=4, K=3, Q=2,
//      J=1). A 2NT opening on this scale might be 20-22, 20-21, 21-22, or
//      similar depending on agreement. We use 20-21, in line with the
//      'Standard American Yellow Card' system:
//      http://web2.acbl.org/documentlibrary/play/SP3%20(bk)%20single%20pages.pdf
//      Expert players may adjust hand valuation up or down slightly depending
//      on the location of their high cards; we do not attempt to replicate
//      this.
//
// The 2NT variant is both a smaller game, and also a fairer comparison
// with existing bots, since in practice auctions which start with 2NT are
// almost always uncontested. This means that bidding is generally conducted
// without worrying that the opponents might bid. This is in contrast to the
// full game where many bids are designed in anticipation of the possibility
// that the opponents may bid - a constraint that we do not have in this game.
//
// We treat the initial deal as a single sampled stochastic chance event; that
// is, the game tree has a single chance event with a single possible outcome,
// but when applying this outcome, the game state evolves stochastically,
// reflecting the full deal that has taken place.
//
// We could have explicit chance in case (1), e.g. with one chnce node for each
// card being dealt, but this would be hard in case (2), and we choose to
// treat both consistently.
//
// The score for player 0 will always be the raw point score for the contract
// reached. If the parameter `relative_scoring` is set to true, then the score
// for player 1 will be the score relative to the best-scoring of the possible
// contracts (so 0 if the contract reached is the best-scoring contract,
// otherwise negative).

#include "open_spiel/games/bridge/bridge_scoring.h"
#include "open_spiel/spiel.h"

namespace open_spiel {
namespace bridge {

constexpr int kNumSuits = 4;
constexpr int kNumDenominations = 1 + kNumSuits;
constexpr int kMaxBid = 7;
constexpr int kNumBids = kMaxBid * kNumDenominations;
constexpr int kNumActions = kNumBids + 1;
constexpr int kNumCardsPerSuit = 13;
constexpr int kNumCards = kNumSuits * kNumCardsPerSuit;
constexpr int kNumPlayers = 2;
constexpr int kNumHands = 4;
constexpr int kNumCardsPerHand = 13;
constexpr int kMinScore = -650;  // 13 undertricks, at 50 each
constexpr int kMaxScore = 1520;  // 7NT making
constexpr int kStateSize = kNumCards + kNumPlayers * kNumActions + kNumPlayers;

class Deal {
 public:
  Deal() { std::iota(std::begin(cards_), std::end(cards_), 0); }
  void Shuffle(std::mt19937* rng, int begin = 0, int end = kNumCards) {
    for (int i = begin; i < end - 1; ++i) {
      // We don't use std::uniform_int_distribution because it behaves
      // differently in different versions of C++, and we want reproducible
      // tests.
      int j = i + (*rng)() % (end - i);
      std::swap(cards_[i], cards_[j]);
    }
  }
  Deal(const std::array<int, kNumCards>& cards) : cards_(cards) {}
  int Card(int i) const { return cards_[i]; }
  int Suit(int i) const { return cards_[i] % kNumSuits; }
  int Rank(int i) const { return cards_[i] / kNumSuits; }
  std::string HandString(int begin, int end) const;

 private:
  std::array<int, kNumCards> cards_;  // 0..12 are West's, then E, N, S
};

class UncontestedBiddingState : public State {
 public:
  UncontestedBiddingState(std::vector<Contract> reference_contracts,
                          std::function<bool(const Deal&)> deal_filter,
                          std::vector<Action> actions, int rng_seed)
      : State(kNumActions, kNumPlayers),
        reference_contracts_(std::move(reference_contracts)),
        actions_(std::move(actions)),
        deal_filter_(deal_filter),
        rng_(rng_seed),
        dealt_(false) {}
  UncontestedBiddingState(std::vector<Contract> reference_contracts,
                          const Deal& deal, std::vector<Action> actions,
                          int rng_seed)
      : State(kNumActions, kNumPlayers),
        reference_contracts_(std::move(reference_contracts)),
        actions_(std::move(actions)),
        rng_(rng_seed),
        deal_(deal),
        dealt_(true) {
    if (IsTerminal()) ScoreDeal();
  }
  UncontestedBiddingState(const UncontestedBiddingState&) = default;

  int CurrentPlayer() const override;
  std::string ActionToString(int player, Action action_id) const override;
  std::string AuctionString() const;
  std::string ToString() const override;
  bool IsTerminal() const override;
  std::vector<double> Returns() const override;
  std::string InformationState(int player) const override;
  void InformationStateAsNormalizedVector(
      int player, std::vector<double>* values) const override;
  std::unique_ptr<State> Clone() const override;
  std::vector<Action> LegalActions() const override;
  std::vector<std::pair<Action, double>> ChanceOutcomes() const override;

 protected:
  void DoApplyAction(Action action_id) override;
  void ScoreDeal();

 private:
  // If non-empty, the score for player 1 will be relative to the best-scoring
  // of these contracts. This may be useful to reduce variance, or to provide a
  // signal for how suboptimal the outcome achieved is.
  std::vector<Contract> reference_contracts_;
  std::vector<Action> actions_;
  // This function is used to select possible deals. We repeatedly shuffle the
  // deck until this function returns true. It may always return true if no
  // filtering is required, or it may check that the opening bidder has a
  // balanced hand with 20-21 HCP (a 2NT opener - see above).
  std::function<bool(const Deal&)> deal_filter_;
  mutable std::mt19937 rng_;
  mutable Deal deal_;
  bool dealt_;
  double score_;                          // score for the achieved contract
  std::vector<double> reference_scores_;  // scores for the reference_contracts
};

class UncontestedBiddingGame : public Game {
 public:
  explicit UncontestedBiddingGame(const GameParameters& params);
  int NumDistinctActions() const override { return kNumActions; }
  std::unique_ptr<State> NewInitialState() const override {
    return std::unique_ptr<State>(new UncontestedBiddingState(
        reference_contracts_, deal_filter_, forced_actions_, ++rng_seed_));
  }
  int NumPlayers() const override { return kNumPlayers; }
  double MinUtility() const override {
    return reference_contracts_.empty() ? kMinScore : kMinScore - kMaxScore;
  }
  double MaxUtility() const override {
    return reference_contracts_.empty() ? kMaxScore : 0;
  }
  std::unique_ptr<Game> Clone() const override {
    return std::unique_ptr<Game>(new UncontestedBiddingGame(*this));
  }
  std::vector<int> InformationStateNormalizedVectorShape() const override {
    return {kStateSize};
  }
  int MaxGameLength() const override { return kNumActions; }
  std::string SerializeState(const State& state) const override {
    return state.ToString();
  }
  std::unique_ptr<State> DeserializeState(
      const std::string& str) const override;

 private:
  std::vector<Contract> reference_contracts_;
  std::vector<Action> forced_actions_;
  std::function<bool(const Deal&)> deal_filter_;
  mutable int rng_seed_;
};

}  // namespace bridge
}  // namespace open_spiel

#endif  // THIRD_PARTY_OPEN_SPIEL_GAMES_BRIDGE_UNCONTESTED_BIDDING_H_
